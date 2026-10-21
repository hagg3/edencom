# Player, Input & Camera

## Purpose
Touch handling, the block-pick raycast, player physics/collision, and the camera.

## Important files
- `Classes/Input.mm/.h` — raw touch bookkeeping (5-slot `itouch` array).
- `Classes/Player.mm` (~2,300 lines) — input interpretation, physics, block editing,
  portals, damage. The single biggest gameplay file.
- `Classes/Util.mm` — `findWorldCoords` raycast, SAT polyhedra collision
  (`collidePolyhedra`, `makeBox/makeRamp/makeSide`), math helpers.
- `Classes/Camera.mm` — view matrix.
- `Classes/Joystick.mm` — virtual stick used by the HUD. `Joystick::layoutForScreen()` (I.2, run by
  `Hud::layoutForScreen`) puts the pad 20 points in from the safe rect's bottom-left (`docs/ui.md`).
  `padbounds` (the pad's screen rect) is a
  plain global, mutable from outside this file; the web port's "move controls" feature (audit row
  17/G1, `port/src/seam/Input_web.mm`'s `eden_joystick_*` exports) set it from a drag gesture.
  Those exports are still compiled but no native UI calls them (a native Controls page is ROADMAP W.6).
  `joystickCustomizeMode` (also global, default false) makes `Joystick::update` no-op entirely —
  no movement input, no `joystick_pos` tracking — while that feature has this on, so a
  repositioning drag can never fight real gameplay touch handling; `render()` is unaffected, so the
  pad stays visible (wherever it currently is) while customize mode is on.

## Input layer (`Input.mm`)
`EAGLView` forwards `touchesBegan/Moved/Ended/Cancelled` to the `Input` singleton,
which maintains `touches[5]` slots keyed by `UITouch*`:
current/previous/first positions (`mx/my`, `pmx/pmy`, `fx/fy`), `down` state
(`M_DOWN/M_RELEASE/M_NONE`), `moved`, `movecam` (was this touch dragging the camera),
per-touch block preview state, and hold time `etime`. Consumers (Hud buttons, Player,
Menu) poll this array every frame — there is no event dispatch. `clearAll()` resets on
world load. `keyTyped` feeds the custom GL keyboard (`VKeyboard`).

**On the port** there is no `EAGLView`: SDL events are translated in
`native/src/seam/Input_native.cpp` — mouse buttons and `SDL_EVENT_FINGER_*` become
`eden_input_pointer_event(phase, identity, x, y)` (`port/src/seam/Input_web.mm`), which synthesises
the same `touches[]` slots, so every consumer above is unchanged. Keys go through the keybind model
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

## Block picking — `findWorldCoords(mx, my, mode)` (`Util.mm`)
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

## Player (`Player.mm`)

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
  - Drag → camera look (`yaw += dx·YAW_SPEED`, `pitch += dy·PITCH_SPEED`,
    optionally inverted; pitch clamped).
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
    during it), so chained hops decay rather than build. The Experiments row
    "Advanced movement (bhop)" (`advanced_movement`) removed that cost on the web build only; its
    consumer was a web link-time wrap, so on native the row is **inert** (it still shows).
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
  SAT implementation in `Util.mm` (`collidePolyhedra` + `minTranDist` minimum
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
- **FLY_MODE** (this fork, `FLY_MODE`/`FLY_UP`/`FLY_DOWN` at the top of `Player.mm`): three bools
  enable flight; fire/pickaxe buttons become up/down thrust. They initialise `true`, but the port
  drives them from Settings › Experiments › **Fly mode** (`fly`, default **off**,
  `eden_set_fly_mode`); Space/Ctrl rise and fall. Off is stock behaviour.
- **Crouch** (port addition, `CROUCH_HELD` global + `Player::canFit`): holding crouch (keyboard,
  Ctrl by default, or the HUD button) eases `boxheight` from `1.85` down to `0.925` over 0.25 s
  (`CROUCH_ANIM_TIME`), feet planted, so the player fits through 1-block gaps. Standing back up is
  rate-limited by `canFit()` scanning the slice being added each frame — under a low ceiling the
  animation just freezes at whatever height still fits instead of snapping into the ceiling block
  (the old instant-toggle version could clip the box into terrain and drop the player through the
  world here). The whole feature is gated by a new `bool CROUCH_ENABLED` global (top of `Player.mm`,
  default **`false`**) — `preupdate`'s read site is now
  `CROUCH_ENABLED && (CROUCH_HELD || hud->m_crouch)`, so both the keyboard key and the HUD button are
  inert when it's off. `Hud.mm` additionally wraps the crouch button's hit-test *and* its render call
  in `if (CROUCH_ENABLED)` (`extern BOOL CROUCH_ENABLED`), so the on-screen button is hidden entirely,
  not just non-functional, when disabled. Settings › Experiments › **Crouch mode** (`crouch`) flips it
  via `eden_set_crouch_enabled` (`Input_web.mm`). See
  [port/docs/player-input-camera.md](../port/docs/player-input-camera.md) for the input wiring.

### Rendering
`Player::render` draws nothing of the player in first person; it exists for the held-block
preview and debug markers (`lol`/`lol1`/`lol2` vectors are debug visualization buffers). In third
person (H.12) it draws the **fallback** body only when `!PlayerBodyAvailable()` (Creatures off, or no
`GL_OES_matrix_palette`): `drawCube(..., TYPE_CLOUD, -1)`, which is what reaches `Graphics::drawCube`'s
`THIRD_PERSON && TYPE_CLOUD && buildsize==-1` branch scaling a cube to the collision box
(`0.667 × 1.85 × 0.667`). Stock passed `TYPE_GRADIENT`, so its branch never fired; the `buildsize==-1`
marker keeps a cloud-block build preview 1×1×1. Otherwise the body is a Green creature in `Model.mm`'s
`PLAYER_BODY` slot ([entities-and-creatures.md](entities-and-creatures.md)), posed each frame from
`Player::update` (`!onground && !FLY_MODE` → jump, horizontal speed > 0.5 → walk, else idle; feet at
`pos.y - boxheight/2`, so crouching lowers it) and hidden in first person and while dead.

## Camera (`Camera.mm`)
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
- **Pick origin** (`findWorldCoords`, `Util.mm`): in mode 1 the ray origin slides forward to the pivot
  (`skip = dot(pivot − wp1, dir)`), so reach stays 15 blocks from the player's eye and blocks between the
  camera and the player can neither be picked nor block the ray.
- **Pull-in** (`Camera::updateThirdPerson`, called from `Player::update` after the camera fields are
  written — `Camera::update` runs before `preupdate` and would see last frame's position): marches from
  the pivot toward the eye in 0.25 steps up to 4.0 (`CAM_TP_DIST`, stock's `n=4.0f`); the first block that
  is not air or shallow liquid (`Player.mm`'s preview test; `-1` counts as passable) caps `dist` at
  `max(t − 0.3, 0.6)`. Shrinks instantly, grows back at 8 blocks/s. The body is not drawn while
  `dist < 1`.
- Underwater tint and the ambience beds stay **player**-based, so in third person the screen can show the
  camera above water while the tint says under.
- Gate: `--camera-selftest` (native/README.md).

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
  displayed frame**. Native's loop paces to its fps cap (`eden_main_native.cpp`, interactive
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
