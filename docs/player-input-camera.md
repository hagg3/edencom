# Player, Input & Camera

## Purpose
Touch handling, the block-pick raycast, player physics/collision, and the camera.

## Important files
- `Classes/Input.cpp/.h` — raw touch bookkeeping (5-slot `itouch` array).
- `Classes/TouchLook.cpp/.h` — **post-import (Stage L, 2026-10-23)**: the touch drag camera's
  arithmetic (degrees per window point, smoothing, glide). See "Touch look" below.
- `Classes/Player.cpp` (~2,300 lines) — input interpretation, physics, block editing,
  portals, damage. The single biggest gameplay file.
- `Classes/Util.cpp` — `findWorldCoords` raycast, SAT polyhedra collision
  (`collidePolyhedra`, `makeBox/makeRamp/makeSide`), math helpers.
- `Classes/Camera.cpp` — view matrix.
- `Classes/Joystick.cpp` — virtual stick used by the HUD. `Joystick::layoutForScreen()` (I.2, run by
  `Hud::layoutForScreen`) puts the pad 20 points in from the safe rect's bottom-left (`docs/ui.md`).
  `padbounds` (the pad's screen rect) is a
  plain global, mutable from outside this file; the web port's "move controls" feature (audit row
  17/G1, `port/src/seam/Input.cpp`'s `eden_joystick_*` exports) set it from a drag gesture.
  Those exports are still compiled but no native UI calls them (a native Controls page is ROADMAP W.6).
  `joystickCustomizeMode` (also global, default false) makes `Joystick::update` no-op entirely —
  no movement input, no `joystick_pos` tracking — while that feature has this on, so a
  repositioning drag can never fight real gameplay touch handling; `render()` is unaffected, so the
  pad stays visible (wherever it currently is) while customize mode is on.

## Input layer (`Input.cpp`)
`EAGLView` forwards `touchesBegan/Moved/Ended/Cancelled` to the `Input` singleton,
which maintains `touches[5]` slots keyed by touch pointer (`UITouch*` in stock; **since G.2b-1** a
`const EdenTouch*`, the engine struct in `Input.h`: `location`, `phase`, `timestamp`, `identity`,
`isReal`, passed as `touchesBegan(const EdenTouch* const* touches, int n)` with no `UIEvent`):
current/previous/first positions (`mx/my`, `pmx/pmy`, `fx/fy`), `down` state
(`M_DOWN/M_RELEASE/M_NONE`), `moved`, `movecam` (was this touch dragging the camera),
per-touch block preview state, and hold time `etime`. **Since Stage L** each slot also carries
`lfx/lfy` (last position, float engine points), `ffx/ffy` (where it went down), `ldx/ldy` (look motion
not yet consumed, summed over *every* event before the int truncation) and `looklatched`; the stock
fields keep their meaning for every other consumer. Consumers (Hud buttons, Player,
Menu) poll this array every frame — there is no event dispatch. `clearAll()` resets on
world load. `keyTyped` feeds the custom GL keyboard (`VKeyboard`).

**On the port** there is no `EAGLView`: SDL events are translated in
`port/src/seam/Input_native.cpp` — mouse buttons and `SDL_EVENT_FINGER_*` become
`eden_input_pointer_event(phase, identity, x, y)` (`port/src/seam/Input.cpp`), which synthesises
the same `touches[]` slots, so every consumer above is unchanged. It keeps **one heap `EdenTouch` per
live pointer identity** from begin to end/cancel, so `touch_id` stays a stable pointer and the `==`
matches keep their meaning (a real finger id 0 is valid and the synthetic keyboard/mouse/HUD ids are
negative, so an `int` id could not stand for "no touch"); `isReal` is `identity >= 0`. Gates: the
look/fly/touch selftests and their mutation legs. Keys go through the keybind model
(docs/ui.md "The keybind model") to movement axes, HUD taps and the picker; `SDL_Gamepad` feeds the
same paths. `VKeyboard` is seam-excluded; text entry is `GLW::TextField` (docs/ui.md).

Coordinates arrive in screen points; note the axis swap when they reach the raycast
(`findWorldCoords` swaps mx/my and scales by `SCALE_*` on "iPad"/Retina).

`Input::scr_width`/`scr_height` are the point-space dimensions the Y-flip in
`touchesBegan/Moved/Ended` uses. **MODIFIED FROM STOCK (2026-07-31):** the constructor used to
re-derive them from the same three device constants `EAGLView` had just written into
`SCREEN_WIDTH`/`SCREEN_HEIGHT`; it now *reads* those globals, through
`Input::screenMetricsChanged()`, which the constructor calls and which can also be called again
later. That is what lets the point space stop being fixed for the process lifetime — see the
`IS_IPAD` entry in [conventions-and-pitfalls.md](conventions-and-pitfalls.md) for the whole change.
Equivalent on every profile the original shipped; the one branch whose behaviour it *does* change
(non-Retina iPad, which set `scr_*` to 1024×768 while `SCREEN_*` said 480×320) was already
unreachable, because the iPad branch that would have set those metrics is commented out in
`EAGLView initWithCoder:`. A stale `scr_height` inverts the Y of every touch, so anything that changes
`SCREEN_HEIGHT` owes this call.

## Block picking — `findWorldCoords(mx, my, mode)` (`Util.cpp`)
1. Rebuilds the camera matrix, `gluUnProject`s the touch at depth 0 and 1 → ray.
2. Marches the ray in 1/8-block steps up to 15 blocks.
3. Each step first tests creatures (`PointTestModels`) — a hit stores the creature
   index in the global `fwc_result` and returns.
4. For terrain: precise polyhedron test against the block's actual shape
   (ramps/sides/liquid levels get their real geometry, not a full cube).
5. `FC_PLACE` returns the last *empty* cell before the hit (stored via `offsetdir`
   logic); `FC_DESTROY` returns the hit cell.
Globals `fwc_result` (creature hit index) and `fpoint` (exact hit point) carry side
results — callers read them immediately after the call.

## Player (`Player.cpp`)

### Update pipeline (`preupdate` → `processInput` → `update` → `move` → collisions)
- **`processInput`**: walks the touch slots. Touches on HUD controls were
  already consumed by `Hud::update` (which sets `inuse`). Remaining logic:
  - Short tap (not moved, released quickly) in BUILD/MINE/PAINT/BURN mode →
    raycast → `terrain->buildBlock/destroyBlock/paintBlock/burnBlock` (with a
    collision pre-check so you can't build inside yourself: `Player::test` collides
    the candidate block's polyhedron against the player's box). Since MP.4 these sites
    (and jump-and-build, the double brush, the portal exit's two mines) call
    `MpWorld::intentBuild/Mine/Paint/Burn` instead, which is the same call outside a
    multiplayer session and also sends the changed cells inside one
    ([networking.md](networking.md) "Edits out and in").
  - Drag → camera look: the `movecam` branch, rewritten in Stage L (see "Touch look"); stock's
    `yaw += dx·YAW_SPEED` int arithmetic is kept there as commented archaeology. Pitch is still
    clamped to ±80 in `Player::update`.
  - "Jump and build" assist (`jumpandbuild`) places a block underneath after a jump
    when building below your feet. (There is no long-press-repeat for mine/paint —
    a prior version of this doc claimed one; verified against the code during the
    port's PC-controls audit, 2026-07, and no such mechanism exists in stock. The edit
    fires only on `M_RELEASE`, one block per down/up pair. The port adds a real
    hold-to-repeat itself, as a port-side feature — see WORKING/archive/PORT-STATUS-2026-08-13.md.)
  - **Auto-rejump** (hold jump to bhop): `Player::update`'s re-trigger gate is
    `lastjump != TRUE && !jumping` — `lastjump` is assigned from `jumping` at the
    END of the previous frame (`lastjump=jumping`), not from the jump button, so holding jump
    re-fires automatically ~2 frames after landing (`jumping` clears on landing
    collision in `Player::vertc`, then the gate needs one more frame for `lastjump` to catch
    up). This works out of the box on touch and on the port's keyboard controls —
    no separate mechanism was needed. The 2-frame gap costs lateral speed (ground
    friction `×0.9` plus the jump's own `vel.x*=.9` lateral tax apply
    during it), so chained hops decay rather than build. (The old
    "Advanced movement (bhop)" Experiments row was removed in FT.2 (middle-click pick block also refuses bedrock, FT.1); a Srclike movement mod is planned later.)
  - Creature interactions: tapping a creature in pick mode grabs it
    (`PickupModel`), tap again places (`PlaceModel`); paint mode colors it
    (`ColorModel`); mine mode hits it (`HitModel`); burn sets it on fire.
- **Movement**: `Hud`'s joystick sets `walk_force`/`max_walk_speed`
  (`setSpeed`, ×`SPEED_M 4.5`); physics integrates with `GRAVITY 20`,
  `JUMP_SPEED 6.7`, ice = low-friction sliding (`onIce`), trampolines invert vertical
  velocity, ladders/vines set `climbing` (`CLIMB_SPEED 3`), liquids damp motion and
  set `inLiquid` (with flow push from `getFlowDirection`), autojump option hops
  1-block steps.
- **Collision**: player is a box (`boxbase=2/3` block wide, `boxheight=1.85`);
  `vertc`/`horizc`/`checkCollision` test the surrounding blocks' polyhedra using the
  SAT implementation in `Util.cpp` (`collidePolyhedra` + `minTranDist` minimum
  translation vector). Ramps are walkable because their true sloped polyhedra
  are tested (`onramp`).
- **Damage/health** (`health_option`): lava contact (`takeDamage(.08)/frame`),
  creature hits, TNT; `life` drives the red `flash`; death → `dead`, respawn at home.
- **Portals** (`Player::move`): standing in a portal block sets `inPortal` once and
  calls `Portal::enterPortal`, which finds the *next portal of the same color* and
  returns its position+exit direction; travel is a `warpToPoint` (save+reload) if the
  destination is outside the resident window.
- **`groundPlayer()`** (the landing step every portal/home warp ends in): searches down
  to the first non-colliding spot, then back up while still colliding. The upward search used to
  stop at a literal `y <= 100`; it is `T_HEIGHT + 36` now, which is the same 100 at 64z and the
  difference between landing and being left embedded in rock in a 256-tall world
  (`WORKING/256z-format-backport-plan-2026-08-05.md`, breaker B2).
- **Fly state** (this fork, FM.1; `FLY_MODE`/`FLY_UP`/`FLY_DOWN`/`NOCLIP_MODE`/`FLY_ALLOWED` at the top
  of `Player.cpp`): the state is Off / Fly / Noclip. `FLY_MODE` means "flying" (Fly *or* Noclip), so
  existing readers stay right; `NOCLIP_MODE` marks Noclip (see **Noclip** below). `fly_set_state(0|1|2)` / `fly_cycle_state()` / `fly_get_state()` (declared in
  `Player.h`) own every transition: they clear `FLY_UP/FLY_DOWN`, and a cycle toasts through
  `hud->sb->setStatus` (*Fly ON* / *NOCLIP on* / *Fly OFF*). `fly_cycle_state()` does nothing
  (silently) unless `FLY_ALLOWED`. `Player::reset()` sets Off, so the state is never saved and a
  world load or respawn starts on foot. The Settings › Gameplay row `fly` (default **off**) is
  now **Allow fly** → `eden_set_fly_allowed` (turning it off while flying drops to Off). V and
  gamepad Y call `eden_cycle_fly_mode`; Space/Ctrl rise and fall while flying.
  `eden_set_fly_mode(on)` stays the programmatic switch (on = Fly, ignores *Allow fly*; selftests
  use it); also `eden_get_fly_state/set_fly_state/get_fly_allowed`. Off is stock behaviour.
- **Noclip** (FM.2): `Player::update` branches right after `processInput` and the yaw animation:
  `noclipUpdate(etime)` then `postMove(etime)`, and **never calls `move()`**. `postMove` is the old
  tail of `update` after `move()` (map-edge wall, sky colour, camera, third-person body, touch
  `etime`, ambience), shared by both paths, so a change there reaches noclip too. Because collision
  and every trigger live in `move()`/`vertc()`, noclip skips: push-out, gravity, ladders, swimming
  and flow, ramps/ice, trampoline, lava damage, golden-cube pickup, portal entry, footsteps/landing
  sounds, autojump and the `y<-10` respawn; `hud->underLiquid` stays false (no tint/underwater bed).
  A new trigger added to `vertc`/`move` is skipped by noclip unless added to the branch too.
  - Movement is Srclike: wish = look forward (yaw **and pitch**) × forward input + right ×
    strafe + up × (`FLY_UP` − `FLY_DOWN`). `walk_force` gives only the direction (the keyboard seam
    folds a frame-rate factor into its length; the joystick passes a unit vector); the magnitude is
    `max_walk_speed/(SPEED_M·1.3)` (1 normal, 0.5 walk, 1.3 sprint / full stick), capped so diagonals
    aren't faster. `vel` approaches `wish·NOCLIP_SPEED·FLY_ACCEL_MULT` by `1−e^(−NOCLIP_RATE·t)`:
    frame-rate independent, and the same law is the crisp stop. `FLY_MAX_SPEED` caps |vel| (3D);
    `fly_braking`/`fly_vertical` don't apply. `pos.y` is clamped to `[1, T_HEIGHT+8]`.
  - **Speeds** (`NOCLIP_SPEED` is the Settings row Gameplay › *Noclip speed* since SX.4, 10–100; `NOCLIP_RATE` has none): `NOCLIP_SPEED` 60 b/s (78 sprinting), `NOCLIP_RATE` 10/s.
    Keep `NOCLIP_SPEED·1.3·FLY_ACCEL_MULT` under **100.5 b/s**: `update()` splits a frame while
    `etime·|vel| > 0.1` and clamps `|vel|` to `JUMP_SPEED*15` first. Fly itself cruises at ~97.5 b/s
    with W held at 60 Hz (measured, `--fly-selftest`), i.e. it rides that clamp.
  - `takeDamage` returns at once in noclip (lava, creatures, TNT, command blocks).
  - **Leaving noclip** (`fly_set_state` to anything else, including *Allow fly* turned off) calls
    `Player::unstick()`: while `boxBlocked()` step up 1 block (cap `T_HEIGHT+36`), then zero `vel` and sync `lpos`, only if it moved the player (SRC.0: momentum survives leaving noclip/fly in open air).
    `boxBlocked()` scans only the cells the box overlaps and counts what `vertc` pushes out of (not
    liquids, flowers, doors, portals, golden cubes, ladders/vines), so an exit under water stays
    under water and one beside a wall doesn't climb it. Not `groundPlayer()`: its first loop drops
    the player to the floor. `Player::reset()` clears `NOCLIP_MODE` first, so a load/respawn doesn't
    unstick at the old position.
  - Gate: `--fly-selftest` (`native/README.md`).
- **Crouch** (port addition, `CROUCH_HELD` global + `Player::canFit`): holding crouch (keyboard,
  Ctrl by default, or the HUD button) eases `boxheight` from `1.85` down to `0.925` over 0.25 s
  (`CROUCH_ANIM_TIME`), feet planted, so the player fits through 1-block gaps. Standing back up is
  rate-limited by `canFit()` scanning the slice being added each frame — under a low ceiling the
  animation just freezes at whatever height still fits instead of snapping into the ceiling block
  (the old instant-toggle version could clip the box into terrain and drop the player through the
  world here). The whole feature is gated by a new `bool CROUCH_ENABLED` global (top of `Player.cpp`,
  default **`false`**) — `preupdate`'s read site is now
  `CROUCH_ENABLED && (CROUCH_HELD || hud->m_crouch)`, so both the keyboard key and the HUD button are
  inert when it's off. `Hud.cpp` additionally wraps the crouch button's hit-test *and* its render call
  in `if (CROUCH_ENABLED)` (`extern bool CROUCH_ENABLED`), so the on-screen button is hidden entirely,
  not just non-functional, when disabled. Settings › Gameplay › **Crouch mode** (`crouch`) flips it
  via `eden_set_crouch_enabled` (`Input.cpp`). See
  [port/docs/player-input-camera.md](../port/docs/player-input-camera.md) for the input wiring.

### Rendering
`Player::render` draws nothing of the player in first person; it exists for the held-block
preview and debug markers (`lol`/`lol1`/`lol2` vectors are debug visualization buffers). In third
person (H.12) it draws the **fallback** body only when `!PlayerBodyAvailable()` (Creatures off, or no
`GL_OES_matrix_palette`): `drawCube(..., TYPE_CLOUD, -1)`, which is what reaches `Graphics::drawCube`'s
`THIRD_PERSON && TYPE_CLOUD && buildsize==-1` branch scaling a cube to the collision box
(`0.667 × 1.85 × 0.667`). Stock passed `TYPE_GRADIENT`, so its branch never fired; the `buildsize==-1`
marker keeps a cloud-block build preview 1×1×1. Otherwise the body is a Green creature in `Model.cpp`'s
`PLAYER_BODY` slot ([entities-and-creatures.md](entities-and-creatures.md)), posed each frame from
`Player::update` (`!onground && !FLY_MODE` → jump, horizontal speed > 0.5 → walk, else idle; feet at
`pos.y - boxheight/2`, so crouching lowers it) and hidden in first person and while dead.

## Camera (`Camera.cpp`)
Follows the player exactly: `px/py/pz` = player pos + eye height; `update` smooths
nothing (direct copy); `render` applies pitch/yaw rotations then translates by
`-(pos − chunkOffset·16)` and hands the view-projection to `setFrustum` for culling.
`render2` reproduces the matrix for the picking unproject (in absolute coords: no `chunkOffset`,
because `findWorldCoords` walks absolute blocks). `reset()` on world load.

**Third person (H.12).** `int CAMERA_VIEW` (`Globals.h`; 0 first person, 1 behind, 2 front) is the
runtime switch; `THIRD_PERSON` (`Constants.h`) is `(CAMERA_VIEW!=0)` and `Player::update` copies the view
into `cam->mode`. Stock's four `THIRD_PERSON` sites were all broken (eye 6 below the player, a frustum from
the first-person lookAt, a picking camera that was a *different* camera, pitch forced to 0); they are
repaired rather than replaced:
- `Camera::viewPoints(eye, at)` is the single source of the (eye, target) pair. The **pivot** is the
  first-person eye (`px/py/pz`); the view direction is the engine's own non-unit
  `(cosYaw·(1−|sinP|), sinP, sinYaw·(1−|sinP|))`, normalized (`viewDir`). Behind: eye = pivot − d·dist,
  looking at the pivot. Front: eye = pivot + d·dist, looking back. `render()` feeds the pair to the
  frustum **and** the scene (the scene minus `chunkOffset` on x/z), so they cannot disagree.
- Behind, the eye sits **on** the first-person ray, so the screen-centre ray is the first-person ray:
  aim does not move when the view toggles. `render2()` uses `viewPoints()` for mode 1; mode 2 falls
  through to the first-person matrix (the front camera looks back at the player, so taps and the
  crosshair aim along the player's own look).
- Chunk face culling and the transparent back-to-front sort use the same eye (`g_cullEye`, set once per frame in `Terrain::render`; Q.1 F19, `docs/rendering.md`). Before, they used the pivot, so a strip of camera-facing faces could be culled when eye and pivot were on opposite sides of a chunk plane.
- **Pick origin** (`findWorldCoords`, `Util.cpp`): in mode 1 the ray origin slides forward to the pivot
  (`skip = dot(pivot − wp1, dir)`), so reach stays 15 blocks from the player's eye and blocks between the
  camera and the player can neither be picked nor block the ray.
- **Pull-in** (`Camera::updateThirdPerson`, called from `Player::update` after the camera fields are
  written — `Camera::update` runs before `preupdate` and would see last frame's position): marches from
  the pivot toward the eye in 0.25 steps up to 4.0 (`CAM_TP_DIST`, stock's `n=4.0f`); the first block that
  is not air or shallow liquid (`Player.cpp`'s preview test; `-1` counts as passable) caps `dist` at
  `max(t − 0.3, 0.6)`. Shrinks instantly, grows back at 8 blocks/s. The body is not drawn while
  `dist < 1`.
- Underwater tint and the ambience beds stay **player**-based, so in third person the screen can show the
  camera above water while the tint says under.
- Gate: `--camera-selftest` (native/README.md).

## Touch look (`Classes/TouchLook`, Stage L, 2026-10-23)
**MODIFIED FROM STOCK.** Stock's `movecam` branch turned by the *last* event's int `pmx − mx` at
0.8°/engine point after a 15-engine-point dead zone (`cDist`) re-tested every frame. Measured on an
Air 2 (L.0, `WORKING/touch-camera-plan-2026-10-17.md`): 6.5% of a medium drag lost when two moves land
in one frame (D1), 37% zero-turn frames on slow drags (D2), speed varying with aspect and UI scale (D3),
a ~27 iOS pt dead zone (D4), and a drag sweeping back past its start stopping there (D5). Now:
- **Every event counts**: `Input.cpp` adds each move's float delta to `ldx/ldy`; `touchlook_step`
  consumes it.
- **Degrees per window point** (an iOS pt): `TOUCH_PT_SCALE` (window pt per engine pt) is written by
  `DisplayProfile.cpp` on every display apply, with `TOUCH_LOOK_DEG_PER_PT` 0.5 on a tablet and
  0.65 on a phone (window short side < 500 pt). `TOUCH_LOOK_SENS_X/Y` multiply it (1; the Settings › Controls sliders `touch_look_speed` / `touch_look_speed_y`).
- **One slop for tap and look**, `TOUCH_SLOP_PT` = 12 window pt, Chebyshev from `ffx/ffy`. Inside it
  `ldx/ldy` are discarded (no jump on crossing; only the travel past the edge turns). Crossing it
  clears `placeBlock` (no tap) and **latches** `looklatched` until release.
- **Smoothing**: a One-Euro filter on the finger's *position* in window points (`TOUCH_LOOK_SMOOTH`,
  0 = bypass = raw exactly, default 0.2; min cutoff 30 Hz → 1 Hz as it goes 0 → 1, beta 0.05).
  Filtering position conserves the total; a latched look steps every frame, events or not, so it
  settles when the finger stops, and the release (`touchlook_end`, in the `M_RELEASE` loop, which runs
  *before* the look loop) consumes that frame's motion and flushes the remainder. Stock dropped the
  release frame's motion.
- **Glide** (`TOUCH_LOOK_GLIDE`, a float half-life in seconds, Settings › Controls slider 0–0.6,
  default 0 = off): on release the speed estimate keeps turning the
  camera, halving every `TOUCH_LOOK_GLIDE` s, stopping under max(3°/s, 5% of start); a touch-down or turning it off
  stops it.
- L.7: the `num > 1` rule clears `movecam` only on touches not yet `looklatched`; before, a tap
  landing on a second finger permanently killed the dragging finger's look (`--look-mutate=clearlatch`).
- Unchanged: `inuse`, `movecam`, the `num > 1` rule (a second not-yet-looking world finger gives up the camera), invert
  (`invertcam` negates both axes as before), the pitch clamp, mouse and gamepad look.
- Gate: `--look-selftest` steps 0–9 plus `--look-mutate=` legs (native/README.md). `TOUCH_LOOK_MUTATE`
  bits are test-only hooks in `Input.cpp`/`Player.cpp`/`TouchLook.cpp`; 0 in every real run.

## Srclike movement, overview (Stage SRC, closing pass)

Experiments › *Srclike movement* (default off) replaces `Player::move()`/`vertc()` with a CS:S-flavoured fixed-tick
model: strafe jumping, bunnyhopping (press-edge jump, *Auto-bhop*, mouse wheel jumps), surfing on any ramp, swimming,
crouch, Source-style camera. Off = byte-identical stock. Clean-room from formulas (`port/tools/srclike-pm-ref.py`
is the reference model; `fixtures/srclike-pm/vectors.txt` its output). Sections below, in order: input (SRC.2),
camera (SRC.7), hull trace (SRC.3), the tick (SRC.4; water/ladder/ice/trampoline SRC.6), side effects (SRC.8).
Tuning: `move.*` cvars = *Srclike settings* sliders. Not built: jump-duck / unduck-needs-room (SRC.5). Gate:
`--srclike-selftest` (legs `input trace move effects special`), `--ui-selftest`, `--console-selftest`.

## Srclike movement, input half (`Classes/SrclikeMove`, Stage SRC.2, 2026-10-31)

Experiments › *Srclike movement* (`srclike_movement`, default off) and its child *Auto-bhop (hold jump)*
(`srclike_autobhop`) reach `SrclikeMove::setEnabled/setAutoBhop` from `Settings.cpp`. Plan:
`WORKING/srclike-movement-plan-2026-10-31.md`. Since SRC.4 the mode moves the player with its own fixed tick
(next sections); everything is byte-identical when it is off.
- **usercmd** (`SrclikeMove::UserCmd`): raw ±1 forward/side from `eden_set_move_input` (`setKeyAxes`, before the
  frame-rate scaling `setSpeed` gets), jump, duck, yaw/pitch; the frame driver builds one **per tick** (`lastCmd()`).
- **Jump** is a fresh press, never the held level: `takeJump(held)` = rising edge, or held with Auto-bhop, or a
  wheel pulse; it runs once per tick and consumes the pulse, so a notch in the air is spent (a frame with no tick
  keeps it for the next tick). The tick jumps only when grounded. Autojump (`autojump_option`) is ignored in the mode.
- **Wheel**: a captured-mouse notch goes through `eden_wheel_action` (`Input.cpp`): jump pulse in the mode (not
  while flying/noclip), else `eden_hotbar_scroll`. Gamepad shoulders and number keys still change the hotbar (`Classes/Hotbar`, `docs/ui.md` "Hotbar model"; scroll skips empty slots).
- **Crouch** is implied: `wantCrouch` ORs `SrclikeMove::enabled()` with *Crouch mode*.
- **`move.*` cvars** (SX.6: bridged from the `move_*` Settings rows; `bridgeMove` is gone): accelerate 5, airaccelerate 100, friction 4,
  stopspeed 75, gravity 800, maxvelocity 3500, tickrate 66.67 (SRC.1 §4.1). Read by SRC.4.
  **SRC.8b:** each is also a Settings row `move_*` (group "Movement tuning", `Settings.cpp`), shown as sliders on the
  Settings sub-page `srclike` opened from Experiments › *Srclike settings* (since SX.1 a `kPages[]` page placed by
  `page "srclike"`, renamed from Movement tuning in SX.4; Reset is the page's titlebar *Defaults*; `docs/ui.md`).
  One store: the row applies into `SrclikeMove::tunables()` and persists as `move_*` prefs. **SX.6:** the rows are
  the cvars' one range table: `eden_settings_cvar()` names a "Movement tuning" row `move.<key less move_>`, so
  `bridgeSettings` registers it like any row (Console.cpp's duplicate range table `bridgeMove` was deleted).
  Gate: `--ui-selftest` (Srclike settings leg).
- Gate: `--srclike-selftest` (leg `input`).

## Srclike movement, camera (Stage SRC.7, 2026-11-03)

In the mode (and not flying/noclip) `Player::postMove` sets `cam->py = SrclikeMove::eyeY(player)` instead of
`pos.y + 0.3·boxheight`:
- **Eye (C2):** feet + `eyeHeight` = 64 u standing (1.644 blocks; stock 1.48) down to 28 u ducked, lerped by the
  hull's own ease (`boxheight`), so the eye follows `Player::preupdate`'s crouch animation. SRC.5 will make an
  in-air duck hold the eye in world space.
- **Step smoothing (C4):** a grounded tick that changes height by 2..19 u adds the opposite offset to the eye,
  eased out at 180 u/s (~0.1 s for a full step). Eden has no block that makes such a step (ramps are slopes), so
  the selftest cannot exercise it; it matters once SRC.6 adds partial shapes.
- **Pitch (C5):** `SrclikeMove::kPitchLimit` = 89 in the mode's `Player::update` branch (stock 80).
- **Sensitivity (C6):** Experiments › *Srclike sensitivity* (`srclike_sens`, child of the mode, 0 = off). When
  > 0, `eden_apply_look_delta` turns `value × 0.022` degrees per count on both axes (no iPad scale), so a CS
  player can copy their `sensitivity`. Not verified: that SDL relative mode delivers unaccelerated deltas on
  Windows (SRC.9 feel round).
- **C1/C3/C7:** already true: look applies per frame in `processInput`, the tick samples yaw/pitch, `Player::pos`
  is the interpolated tick position, and the engine has no view bob or landing punch.
- Gate: `--srclike-selftest=move` (C2 eye heights, C5 clamp, C6 conversion).

## Srclike movement, hull trace (`Classes/SrclikeTrace.cpp`, Stage SRC.3, 2026-11-01)

`SrclikeMove::traceHull(start, end, mins, maxs)` sweeps an axis-aligned hull through the resident terrain and
returns `fraction`, `endpos`, the hit plane `normal`, `startsolid`/`allsolid`, `ramp` (slope plane, 0.7 ≤ nz <
0.99: the surf rule) and the block `type` + cell. It is the port of `port/tools/srclike-pm-ref.py`'s `trace_hull`
(SRC.1 oracle): expanded-plane brush clip, `DIST_EPSILON` 0.03125 u back-off, "moving away" = `d2 >= d1 − 1e-6`
(a slide along a plane you rest on is a miss), exact contact = start-solid.
- **Units and axes:** Source units, **z up**, **double**: `S = (E.x, E.z, E.y) × 72/1.85` (`toSource`/`toEden`),
  so Eden's `(x, z, y)` terrain order is Source `(x, y, z)`. Double because world coordinates reach ~112 000 u,
  where a float step is a quarter of `DIST_EPSILON`; SRC.4 keeps its state in double too.
- **Shapes** (`blockShape`, `blockBrush`): cube = 6 axial planes; ramp/side-ramp = those plus one 45° plane through
  the cell centre (slope, or vertical diagonal facing the missing corner). Orientation copies stock
  `makeRamp`/`makeSide` as `Player::vertc` calls them (`type%4`); the selftest compares vertices for all 32 ids.
  **Not solid:** air/invalid (`-1`, above/below the world), all liquids (lava too: SRC.6 decides), doors, flowers,
  portals, golden cubes (stock walks through doors/flowers; portal one-sidedness and pickups are SRC.8). Ladders,
  vines, trampolines and palette/missing blocks are cubes.
- Cells come from `getLandc2` (GBLOCK, toroidal; negative x/z skipped). Cost ≈ 0.2–0.4 µs per trace on the Mac.
- Gate: `--srclike-selftest=trace` (14 checks, room built on the toroidal wrap seam `x = z = k·T_SIZE`).

## Srclike movement, the tick (`Classes/SrclikeMove.cpp`, Stage SRC.4, 2026-11-03)

`Player::update` hands the whole step to `SrclikeMove::frame` when the mode is on and the player is not flying
(before the stock substep clamp; `SrclikeMove::invalidate()` otherwise). Stock `move()`/`vertc()` never run in
the mode, so its side effects (footsteps, pickups, portals, lava, liquids, ladders, ice) are absent until
SRC.6/SRC.8; `processInput` (build/mine) and `postMove` (edge wall, camera, sky, ambience) still run.
- **Core** (`pmTick`, `PmState`): a line-for-line port of `port/tools/srclike-pm-ref.py`'s `Player.tick` minus water
  and duck. Jump SETS vz 268.33 on the press tick, which skips friction (the bhop); leapfrog gravity (half before
  the move, half after); ground = friction + accelerate + walk move (step move = plain slide vs up-18 u/slide/down,
  then stay-on-ground); air = air-accelerate (30 u/s wish cap, rate from the uncapped wish) + slide move (4 bumps,
  5 planes, same-plane nudge); categorize (2 u probe, `vz > 140` never ground, the **surf rule**: a ramp plane is
  ground only if grounded last tick or slow, `|v_h| ≤ 1.1·walk`, `|vz| ≤ jump`). State is **double, Source units,
  z up**; `Player::pos` is written in float only at the end.
- **Wish:** `forward`/`side` in Eden's sense: forward `(cos yaw, sin yaw)`, side `(−sin yaw, cos yaw)` in Source
  x/y (= Eden x/z, the stock `cosYaw90` direction), times `kWalkSpeed` (227.68 u/s = 5.85 b/s) × `speedMul`
  (`max_walk_speed / (SPEED_M·1.3)`: sprint 1.3, Alt-walk, a stick's magnitude). With no key axes the stick/touch
  direction comes from `walk_force` (D8).
- **Fixed tick + interpolation** (D2): `frame` adds `etime` to an accumulator (capped at 8 ticks: a hitch is slow
  motion, not a burst), runs `pmTick` every `1/move.tickrate`, then writes `Player::pos` = the previous tick origin
  lerped toward the current one by the leftover fraction, and `Player::vel` = the tick velocity. So the eye lags
  the simulation by up to one tick, and picks/builds use what is drawn.
- **Re-seed:** the driver remembers the `pos`/`vel` it wrote. A different `pos` (teleport, portal, respawn, world
  edge clamp; not crouch, which keeps the feet) re-seeds the state from Player; a different `vel` alone is adopted
  (leaving noclip keeps its momentum, SRC.0). A seed that is start-solid (stock rests the feet exactly ON a floor,
  which the trace calls start-solid) is lifted the least of `k·DIST_EPSILON` / 1–18 u that frees it, else stock
  `unstick()`.
- **Hover after landing** (model and Source behaviour): a landing tick that ends within the 2 u probe grounds the
  hull where it is, and standing still never snaps it down (stay-on-ground runs only while moving), so a hull can
  rest up to 2 u (0.05 block) above a floor; a jump from there peaks that much higher.
- **Float `Player::pos`:** at x ≈ 65 500 a float step is 0.004 block, 4–10 % of one frame's walk travel; the
  double state is exact, the drawn position is quantised as in stock.
- Cost: ~0.5 µs per tick on the Mac (surfing).
- Gate: `--srclike-selftest=move` (13 checks): the SRC.1 vectors (`port/tools/fixtures/srclike-pm/vectors.txt`, read
  at run time, `--srclike-vectors=PATH`; `srclike-pm-ref.py --check` in CI keeps it the model's output) within 1 %
  (absolute floor 0.05, so tick counts are exact), each `sc_*` scenario rebuilt in voxels; then real frames: 66.67
  ticks/s under 60 Hz, smooth interpolation at 60/144 Hz, teleport re-seed, velocity adoption, a jump.

## Srclike movement, side effects (Stage SRC.8, 2026-11-03)
`SrclikeMove::frame` moves the player but runs none of what `Player::move()`/`vertc()` do around it, so the
mode's branch in `Player::update` calls `Player::srclikeEffects(etime, wasOnground, preVelY)` after it, once per
frame: footsteps (same cadence as `move()`), landing sound and `jumping=false` on an air→ground transition after a
fall (`jumping || preVelY < -1`), golden-cube pickup (hull-overlap scan; scenery in an MP session, MP.3a), lava
burn (`takeDamage(.08)` + `S_LAVA_BURN` while the feet are below a lava cell's surface), portal entry (the same
`pos + heading/5.3` probe for `TYPE_PORTAL_TOP`; the frame driver re-seeds from the moved pos/vel next frame) and
the `pos.y < -10` / `> 3·T_HEIGHT` pops. `postMove` already did the edge wall and camera. **Not here (SRC.6):**
splash, `underLiquid`, swimming, ladders, ice, bounce blocks (the trace treats liquids as non-solid).
Third-person body animation reads `vel`, which `frame` writes, so it needs nothing.
**Multiplayer:** `MpWorld` sends `Player::pos/vel` (POSVEL, 4 Hz moving); `ewb-server` has no speed check, only
`MOVE_VEL_MAX = 4096` b/s and position bounds (`hardening.h`), far above `move.maxvelocity` 3500 u ≈ 90 b/s.
Remote clients dead-reckon (`kMaxExtrapolate` 0.6 s, snap beyond 12 blocks), so a 90 b/s surfer is seen with
snaps rather than glide: a live check is owed (user).
- Gate: `--srclike-selftest=effects` (E1 cube pickup, E2 lava, E3 landing, E4 world pop).

## Common pitfalls
- The same touch can be claimed by HUD, camera-look, and block-edit logic; the
  `inuse`/`movecam` flags are the fragile arbitration — test on device with
  multi-touch when changing anything here.
- `findWorldCoords` mutates GL matrix state (loads identity into MODELVIEW) — call it
  only outside the render passes.
- Physics is frame-rate-dependent in places despite `etime` (forces tuned at 60 fps;
  the 1/20 s clamp in `World::update` is the guard; the native loop also clamps to 0.1 s before
  calling it). That clamp guards only the *slow*
  side. **The fast side has no guard, and walking dies there** (measured 2026-09-25, N.4.7):
  ground friction is per-tick (`vel.x/z *= .90`) while acceleration is `etime`-scaled, so
  steady walk speed is ≈ `9 · 35 · etime` under the `max_walk_speed` clamp. On flat stone
  with the stick at full forward that came to 3.6 u/s at 60 ticks/s, 2.6 at 120 and 1.7 at 240.
  At ~540 ticks/s it was **exactly 0**: positions near the map centre are float32 at ≈ 65536,
  where one ULP is 1/128 block, and a sub-half-ULP `pos += vel*etime` rounds to no movement
  (`vel` stays non-zero, so nothing looks stuck). Fly mode (`*= .995`, ×10 speed) clears the
  ULP, which is why "fly works, walking doesn't" means *tick rate*, not touch delivery or
  `onground` (that read 120/120 throughout). A host must feed the engine **one tick per
  displayed frame**. Native's loop paces to its fps cap (`port/src/harness/`, interactive
  loop) instead of running update-without-draw (the retired web build got that free from rAF).
  The touch joystick's `setSpeed` is not frame-rate-normalised on native (the keyboard path is).
- `Player::test` uses `hud->blocktype` implicitly — it tests the *candidate* block
  shape, not a generic cube.

## Default-seed edge clamp (X.6)
`Player::update` pins default-seed (`LEVEL_SEED==DEFAULT_LEVEL_SEED`) players to the bundled square, blocks
`[4096*CHUNK_SIZE-GSIZE/2, +GSIZE]` on x and z. It is skipped when `FileManager::worldUnbounded` (world data outside
that square, set at load; see save-load.md). Fresh worlds are unchanged (checksum modes depend on the clamp).
Beyond the map unsaved columns are void; walking off falls to the respawn.
H.4: a Classic world made with Size "2880 square" (`classic.size == 1`) gets the same clamp, and its
generator leaves columns outside the square void, so it ends where the bundled map does.

## Safe vs. risky to modify
- **Safe:** speed/jump/gravity constants, fly mode, damage values, adding new
  HUD-mode actions in `processInput`'s mode switch.
- **Caution:** the touch-arbitration flags, collision epsilon handling
  (`minTranDist` thresholds), portal warp flow (it re-enters save/load).

## Srclike movement: liquids and specials (SRC.6)

In the mode, water levels 2-3 use a swim move (no gravity, jump held rises at 107.66 u/s, pitch steers); a wall at the
feet with clear air above lets forward trigger a water-jump. Ladders/vines climb only with move input. Ice cuts ground
friction to a tenth, trampolines rebound a landing elastically (at least 0.6 of a jump), flowing water pushes. Detail:
`WORKING/srclike-movement-plan-2026-10-31.md` §11; surf map: `docs/surf-test-map.md`; gate: `--srclike-selftest=special`.
