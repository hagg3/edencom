# Player, Input & Camera (Port Layer)

> First written for the web build (retired 2026-10-13). Its browser input layer (Pointer Lock, the
> `eden-st.html` click/relock fixes, `PointerEvent` profile detection, the DOM hotbar and crosshair)
> is at tag `web-port-final` ([web-port-archive](../../docs/web-port-archive.md)).

Read [`../../docs/player-input-camera.md`](../../docs/player-input-camera.md) first —
`Input.mm`'s touch-slot model, `findWorldCoords`'s raycast, and `Player.mm`'s
physics/collision are the engine's. This file covers the input layer the port adds on top.

## Desktop and touch input (`native/src/seam/Input_native.cpp` → `port/src/seam/Input_web.mm`)
SDL events are translated in `Input_native.cpp` and reach the engine through the shared C accessors
in `Input_web.mm` — `eden_input_pointer_event` (synthetic touches into the `Input` singleton),
`eden_set_move_input`, `eden_apply_look_delta`, `eden_click_begin/end`, the hotbar and picker calls.
Keys are looked up in the keybind model (`kKeybinds[]`, [ui.md](ui.md)); defaults: WASD, Shift sprint,
mouse-look under SDL relative mouse mode (`SDL_SetWindowRelativeMouseMode`, captured while playing
and released whenever `eden_ui_wants_cursor()`), left click mines, right click builds, middle click
eyedrops (below), wheel + digits 1–9 for the hotbar slots, Space jump, Ctrl crouch (held; a no-op
unless the "Crouch mode" setting turned on `CROUCH_ENABLED`), F fly, B block preview, `[` `]` picker
pages, Escape menu/back. A held click repeats the edit every 200 ms (the hold-to-act pulse). Touch:
`SDL_EVENT_FINGER_*` become touches directly; the touch profile shows the on-screen joystick and
buttons.

**Hotbar without a strip.** The 9 slots (`kHotbarBlocks[]` in `Input_web.mm`, fed by the real picker)
select `hud->blocktype` and `MODE_BUILD`, but nothing draws them on native — the strip and the
crosshair were DOM overlays. The Settings › Interface "Crosshair" row is likewise inert on native.

## The Y-axis coordinate gotcha
`eden_input_pointer_event(phase, id, x, y)` takes **top-left-origin, Y-down**
coordinates (the UIKit `-locationInView:` convention `Input.mm` expects) — but
`Input.mm` flips Y (`scr_height - point.y`) before storing it, so a `Menu.mm`
`Button` rect's Y range ends up **bottom-left-origin, Y-up**. To synthesize a tap
inside a rect at flipped-space `[y0, y0+h]`, send raw
`y = SCREEN_HEIGHT - (y0 + h/2)` — do not pre-divide by `SCALE_WIDTH`/`SCALE_HEIGHT`
before sending; the engine does that scaling itself.

## Native: a text field takes the keyboard mid-hold (`native/src/seam/Input_native.cpp`, D.4b)

**A press that opens a GL text prompt must not leave anything held behind.** The SIGN tool opens
its prompt from a right-click; the prompt frees the cursor (`eden_ui_wants_cursor()` is 1 under a
`GLDialog`), so the button's release arrived in cursor mode and went to the menu pointer path, and
the hold-to-act pulse (`g_hold`) never ended: every 200 ms it built again and the prompt re-opened
(the user on the Mac, 2026-10-10 — the only way out was F, and then the fire tool repeated). The
field also swallowed key-ups, so a key held at that moment kept walking the player after the prompt
closed. Fixed three ways: `eden_text_input_start` calls `release_held_input()` (ends the pulse,
clears every `g_down`, drops the jump touch); `MOUSE_BUTTON_UP` ends a matching hold whatever the
capture state; `KEY_UP` is no longer filtered while a field is up (releasing a key that isn't down
is a no-op). And in the engine both picker tools ignore a tap while a dialog is up. Gate:
`--signs-selftest`'s two right-click legs, with **the button held 6 frames past the prompt** — a
release in the very frame the prompt opened is processed before capture drops and hides the bug,
which is exactly how the first draft of the gate passed with the bug put back.

## Native touch: SDL's second copy of every finger (`native/src/seam/Input_native.cpp`)

**`SDL_HINT_TOUCH_MOUSE_EVENTS` defaults to `"1"` on every SDL platform, so on a touchscreen every
finger also arrives as a mouse** — `MOUSE_MOTION`/`BUTTON_DOWN`/`BUTTON_UP` with
`which == SDL_TOUCH_MOUSEID`. The translator forwards fingers to `eden_input_pointer_event()` *and*
forwarded that second copy to the desktop mouse path, which while playing (`g_relativeMouse`) is
mouse-look and the hold-to-mine slot. On a real iPad that read as three separate bugs
(2026-09-06 device pass, STATUS §2.0.12):

- **one tap broke a block in two places at once** — the finger path edited where the finger landed,
  the mouse path's hold-to-act edited at the crosshair;
- **the joystick was "broken"** — dragging the on-screen stick fed `eden_apply_look_delta`, so the
  camera swung while you were trying to walk (one 200×120 synthetic delta moves yaw 160° and pins
  pitch at its −80 limit);
- **and pushing forward "nudged a step at a time" rather than walking** is what that looks like from
  the player's side, because the forward vector is re-aimed every frame the finger moves.

The fix is a `which`-field guard on all four mouse cases plus the reverse guard on fingers
(`SDL_MOUSE_TOUCHID`), and the hints turned off in `eden_native_input_init`. **The guards are the
fix and the hints are politeness** — a hint is a request.

*Two things worth keeping from how this was found.* The gate that was supposed to cover touch
(`--touch-selftest`) passed on all four platforms throughout, because `SDL_PushEvent` synthesises
nothing: a test that pushes its own events can never see what the platform adds to real ones. And
the first version of the new assertions *also* passed with the fix removed — they ran with the
in-game menu open, where `g_relativeMouse` is false and the mouse path is switched off anyway. A
gate has to be run once with the bug put back.

## Input-mode detection
Touch vs. keyboard+mouse is `eden_effective_input_is_touch()` (`Settings_web.mm`): the `input_mode`
setting (hidden on native) or the detected flag, which native sets **once at boot** — touch on iOS,
desktop elsewhere unless `--touch-profile` (`eden_set_detected_touch` in `main()`). It is not
re-detected from real input on native (the web build switched on `PointerEvent.pointerType`). The
profile drives the layout ([ui.md](ui.md) "The point space"); gates: `--touch-selftest
--touch-profile`, `--display-selftest`.

## Gamepad
The web build's `eden-gamepad.js` translator (browser Gamepad API → the keybind action vocabulary) is retired. Native gamepad input is `native/src/seam/Input_native.cpp` (SDL3), gated by `--gamepad-selftest`; button mapping and numbers are in `../../native/README.md`. `Classes/Gamepad.mm` (2010 iOS MFi plumbing) is still untouched.

## Frame-rate-dependent movement
`Player::setSpeed` reads `walk_dir`'s MAGNITUDE as the target velocity, so a unit-length direction means a player who does not move. `eden_movement_scale_walk_dir()` (`src/seam/Movement_web.mm`: `fps_normalize` setting × `walk_speed`) is the one implementation; `eden_set_move_input` (`Input_web.mm`) applies it on the keyboard/gamepad path. (The web build also `--wrap`ped `Player::setSpeed`/`preupdate` for the touch path and the opt-in bunny-hop; those wraps were removed in W.1.)

## FOV
`Classes/Graphics.mm` passes `eden_fov_degrees` (the FOV setting; 80 when unset) to `gluPerspective`. Picking follows for free, because `Util.mm`'s unproject reads the resulting projection matrix back rather than assuming 80°.

## Crouch touch button
A second HUD button next to Jump (`Hud.h`/`.mm`: `rcrouchrender`/`rcrouchhit`/`m_crouch`) drives
crouch on touch, following the same touch-derivation pattern as `m_jump` — never written directly,
re-derived from live touches inside `rcrouchhit` every frame, same as the rule above. It reuses the
Jump icon texture mirrored top-to-bottom (`Texture2D::drawButton`/`drawText`'s new `flipX` param —
edited in **both** `Classes/Texture2D.mm` and this port's seam replacement,
`src/seam/Texture2D_web.mm`, since Texture2D is fully seam-excluded here; missing either half is a
link-time undefined-symbol error, not a silent bug). `Player::preupdate` wants crouch when
`CROUCH_ENABLED && (CROUCH_HELD (keyboard) || hud->m_crouch (this button))` — the keyboard and touch
sources are still ORed at the read site rather than having the button write `CROUCH_HELD`, so they
can't stomp each other, but both are now gated by the `CROUCH_ENABLED` global.

Crouch is **off by default** (`CROUCH_ENABLED=false`, top of `Classes/Player.mm`) and is a port-exposed
settings toggle (as fly mode now is too): the "Crouch mode" row in the
"Experiments" settings tab (see [ui.md](ui.md)) calls `eden_set_crouch_enabled()`
(`src/seam/Input_web.mm`, mirroring `eden_set_fly_mode`/`eden_get_fly_mode`), which is wired through
`eden_apply_setting()` in `src/seam/Settings_web.mm`. When it's off, `Hud.mm` also hides the crouch
button entirely — its hit-test loop and its render/draw call are both wrapped in
`if (CROUCH_ENABLED)` (`extern BOOL CROUCH_ENABLED`) — so there's no dead button sitting on screen,
matching how `FLY_MODE` hides its own up/down affordances when off.

## Camera view (H.12)
Experiments › *Camera view* (`camera_view`, `KIND_ENUM` 0–2) applies through `eden_apply_setting()` →
`eden_set_camera_view()` (`src/seam/Input_web.mm`, beside `eden_set_fly_mode`), which writes the engine's
`CAMERA_VIEW`. The **F5** `cameraView` keybind (`KB_NATIVE`, resolved as `g_miCameraView` in
`native/src/seam/Input_native.cpp`'s one-shot chain) calls `eden_cycle_camera_view()` (`Settings_web.mm`),
which advances `(v+1)%3` **through `eden_settings_set`** — deliberately not fly mode's pattern, whose key
writes the global and lets the Settings row drift. A new mode-style key should write through its setting
the same way. Gate: `--camera-selftest`.
- **Fly tuning (Settings › Gameplay, moved out of Experiments with *Fly mode*).** Four sliders under `fly`
  (`fly_accel`, `fly_max_speed`, `fly_braking`, `fly_vertical`) set the `FLY_ACCEL_MULT`/`FLY_MAX_SPEED`/
  `FLY_BRAKING`/`FLY_VERTICAL_MULT` globals in `Classes/Player.mm`. Defaults (1, 0 = unlimited, 0, 1) reproduce the
  stock feel bit-for-bit; braking >0 blends the per-frame drag from .995 toward .80 (made frame-rate independent only then).

## Middle-click eyedropper (`eden_pick_block_at_crosshair`, `src/seam/Input_web.mm`)
Middle-click raycasts the crosshair block and copies its type into the current hotbar
slot (`Terrain::getLand`). **Fixed 2026-07-31:** it read type only and dropped the
block's paint color, unlike the real 35-cell picker (`Hud.mm`) which always sets
`hud->block_paintcolor` alongside `blocktype`. Now also reads `Terrain::getColor(x,z,y)`
and assigns it to `hud->block_paintcolor`, so eyedropping a painted block carries the
color into the next placement.

## Port-invented input
The 9-slot hotbar, the middle-click eyedropper, hold-to-act repeat, fly/crouch keys and the
gamepad mapping are port inventions — the original engine has only the scrolling 35-cell picker and
touch controls.
