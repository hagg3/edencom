# Player, Input & Camera (Web Port)

Read [`../../docs/player-input-camera.md`](../../docs/player-input-camera.md) first —
`Input.mm`'s touch-slot model, `findWorldCoords`'s raycast, and `Player.mm`'s
physics/collision are unmodified. This file covers the desktop input layer the web
port adds on top, and two bugs whose fixes span this topic and rendering.

## Desktop input (`src/seam/Input_web.mm`)
Built from scratch, feeding the same `Input` singleton via synthetic touches: WASD,
Shift-sprint, Alt-walk, mouse-look under Pointer Lock (`unadjustedMovement`
requested, separate Y-axis sensitivity), click to mine/build, scroll-wheel + digits
1–9 for the hotbar, E/C/Esc, F to toggle fly, Space to jump, Ctrl to crouch (held,
`eden_set_crouch` — a no-op unless the "Crouch mode" setting has turned on `CROUCH_ENABLED`, see
below), B to toggle a block preview. Pointer-lock edge cases: window blur force-ends a held click; Escape is
guarded against double-acting with pointer-lock's own forced pointer release.

**"3-6 random clicks to mine/build" (fixed 2026-07-30, `public/eden-st.html`):** not an
`Input.mm`/`Hud.mm` consumption bug — `mousedown` only reaches `holdActStart`
(→ `eden_click_begin`) while `pointerLocked` is true, and *acquiring* that lock is
what was unreliable. Chromium enforces a real ~1.2s cooldown on
`requestPointerLock()` shortly after any lock exit (closing a menu/picker with
Escape triggers one), silently rejecting calls made inside it via a
`pointerlockerror` event — no exception, so the old `try/catch` around
`requestLock()` never saw it, and both call sites (manual click-to-lock, and the
auto-relock after a picker closes) were one-shot with no retry. A player clicking
during the cooldown got nothing and had no way to know why; the "3-6" was just how
many blind clicks it took to land one outside the window. Fixed with a
`lockWanted`/`pointerlockerror`-driven retry (bounded backoff, ~250ms × 6) so one
click keeps trying until the cooldown clears instead of requiring the player to
guess.

**"Extra click needed after closing the block/colour picker" (two-part fix,
`public/eden-st.html`):**

- *Part 1 (2026-07-31):* the auto-relock-after-picker-closes path (`trackCursorNeed`,
  driven from `Module.__edenFramePost`, i.e. the engine's per-frame poll) calls
  `requestPointerLock()` from a rAF-adjacent context with no active user gesture — a
  tick after the click that closed the picker, not inside it. Browsers gate pointer
  lock on transient user activation, so that call was liable to be silently rejected
  regardless of the retry loop above (which only survives the *cooldown*, not a missing
  gesture), leaving the player one real click short of getting the lock back. Fixed by
  also attempting the relock synchronously inside the `mousedown`/`mouseup` listeners
  themselves (`reacquireLockIfJustClosed`), gated on `eden_ui_wants_cursor()` reading
  true-before/false-after the touch was forwarded into the engine.

- *Part 2 (2026-07-31, still reproduced after part 1):* that `false-after` read was
  itself always stale, so the gate never actually opened from a click. Forwarding the
  touch (`sendTouch` → `eden_input_pointer_event` → `Input::touchesBegan/Ended`) only
  fills `Input::getInput()`'s touch-slot table synchronously — the thing that reads a
  picker-swatch touch and flips `hud->mode` back out of `MODE_PICK_BLOCK`/
  `MODE_PICK_COLOR` is `Hud::update()` (`Classes/Hud.mm`), which runs only on the
  engine's *next* tick (`World::update()`, driven by `emscripten_set_main_loop`'s
  rAF), never inside the synchronous DOM handler. So `eden_ui_wants_cursor()`, read
  immediately after `sendTouch()` in the same call, still reported the picker as open
  on the exact click that closed it, and `reacquireLockIfJustClosed` bailed out every
  time — silently falling through to the per-frame poll's non-gesture relock (part 1's
  bug, reintroduced by a stale read). Fixed by dropping the post-touch
  `eden_ui_wants_cursor()` check entirely and trusting `wantedCursorBefore` (an
  accurate read from *before* this gesture, past the engine's last tick): if a picker
  was open, attempt the relock unconditionally. A click that doesn't actually land on
  a swatch (picker stays open) just costs one harmless lock/re-exit flicker next
  frame, caught by the per-frame poll's own `wants` check. The JS-owned overlays
  (settings/pause/load-error/main menu) are still gated on their own `isOpen()`, since
  those are plain synchronous JS state, not engine-tick-delayed. The per-frame poll's
  own relock branch remains as a fallback for picker closes that don't originate from
  a click at all (e.g. Escape or an engine-side HUD button).

## The Y-axis coordinate gotcha
`eden_input_pointer_event(phase, id, x, y)` takes **top-left-origin, Y-down**
coordinates (the UIKit `-locationInView:` convention `Input.mm` expects) — but
`Input.mm` flips Y (`scr_height - point.y`) before storing it, so a `Menu.mm`
`Button` rect's Y range ends up **bottom-left-origin, Y-up**. To synthesize a tap
inside a rect at flipped-space `[y0, y0+h]`, send raw
`y = SCREEN_HEIGHT - (y0 + h/2)` — do not pre-divide by `SCALE_WIDTH`/`SCALE_HEIGHT`
before sending; the engine does that scaling itself.

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
Auto/Touch/Keyboard+Mouse mode (a setting, see [ui.md](ui.md)) keys off
`PointerEvent.pointerType` (`'touch'` vs. `'mouse'`/`'pen'`), not a bare `mousemove` —
a bare `mousemove` listener used to misfire on a hybrid touch+mouse device mid-touch
session and yank away the on-screen joystick. The wasm call is de-duped to fire only
on an actual profile change, not on every pointer move.

## Gamepad (`public/eden-gamepad.js`)
A **pure translator**, not a third input path: it polls `navigator.getGamepads()` once per frame
from `eden-st.html`'s existing rAF loop and calls the *same* entry points the keyboard/mouse path
uses — the keybind action vocabulary (`actionDown`/`actionUp`, so momentary-vs-continuous and
auto-repeat handling come for free), the mouse's hold-to-act state machine for the triggers, and
`eden_apply_look_delta` for the right stick. **No new wasm exports and no C-side gamepad code.**
`Classes/Gamepad.mm` is deliberately untouched: it is 2010-era iOS MFi/attachment plumbing, and
wiring a browser API into it would be a platform difference living in engine code (web/CLAUDE.md
rule 2).

- **Ordering is load-bearing.** `EdenGamepad.tick()` must run *before* the loop's
  `recomputeMove()`. The stick does not call `eden_set_move_input` itself — `recomputeMove()` runs
  unconditionally every frame (the F1 fix requires it) and would overwrite it one frame later, so
  the stick axes are exposed via `EdenGamepad.axes()` and **summed and clamped** with the keyboard
  axes in that one place.
- Standard mapping only (`pad.mapping === 'standard'`); anything else is logged once and ignored
  rather than guessed at. Left stick moves, right stick looks (radial deadzone with rescaling, a
  squared response curve, integrated against real dt and clamped so a stalled tab can't snap the
  camera), A jump / B crouch / X block picker / Y fly / L3 sprint / R3 fly-down / Start menu /
  Select settings / D-pad up+down fire+colour tools, LT build and RT mine through hold-to-act
  (only one trigger owns that slot at a time), LB/RB and D-pad left/right scroll the hotbar.
- Three settings rows in `Settings_web.mm`'s `kSettings[]` (`gamepad`, `gamepad_look_sensitivity`,
  `gamepad_deadzone`) — per the port's "add a setting in C, never in JS" rule.
- `tools/headless-gamepad-test.js` loads the real file into a sandbox with a fake
  `navigator.getGamepads()` and a recording bridge, and covers deadzone shaping, edge triggering,
  trigger-slot ownership and every release path. It cannot cover feel, or that a real pad reports
  this button order.

## Frame-rate-dependent movement (fixed via `--wrap`, not an engine edit)
See [execution-flow.md](execution-flow.md) for the full walk-speed fix
(`-Wl,--wrap=_ZN6Player8setSpeedE6Vectorf`, `src/seam/Movement_web.mm`,
`fps_normalize` setting). The same wrap fixes a sibling bug: both call sites into
`Player::setSpeed` pass a **unit-length** direction with `walk_speed` used only as a
ceiling, so the engine's 1.3× sprint multiplier never actually reached the
acceleration term — Shift-sprint was a no-op. Fixed by folding `walk_speed` into the
scaled magnitude the wrap passes through.

An opt-in `advanced_movement` setting (default OFF) wraps `Player::preupdate`
(`_ZN6Player9preupdateEf`) instead, for zero-delay bunny-hop: it captures the
jump-button transition and writes `vel.y`/`jumping` directly on landing-while-held,
skipping the engine's one-frame-lagged rejump gate (`lastjump`, a file-static
otherwise unreachable from outside `Player.mm`).

## Block preview (`B`, opt-in)
`Player::render` (`Player.mm`) already draws a ghost block for any held touch with a
latched `previewtype` — on a real touchscreen that's naturally press-and-hold, but a
mouse click is press+release in one gesture, so without help it only flashes.
`eden_update_block_preview()` works around this by parking a **persistent synthetic
touch** at the crosshair: `inuse` is set directly to `Player`'s own `usage_id` (so
`Hud::update` can never claim the slot for a button), `moved=TRUE`/`movecam=FALSE`
every frame, and it is never released — only `M_RELEASE` actually edits the world,
so a touch that's never released can't itself place a block. Known side effect: a
second live touch trips `Player.mm`'s `num>1` branch and clears `movecam`, disabling
drag-to-look while the ghost is up — a free tradeoff on desktop, a real one on a
touchscreen.

Any `hud->` flag more generally is re-derived from touches every frame — never write
one directly from web-side code (see
[conventions-and-pitfalls.md](conventions-and-pitfalls.md) #2).

## Mobile touch-offset / picking-viewport fix
`findWorldCoords`'s raycast reads back whatever `GL_VIEWPORT` currently reports and
assumes a fixed `SCALE_WIDTH`/`SCALE_HEIGHT` of 2.0. This is really a rendering-side
fix (`GL_VIEWPORT` answers the engine's POINT space × `SCALE_*` for picking purposes only,
never the real drawable) but it's exactly this subsystem's bug — see
[gl-shim.md](gl-shim.md) for the full explanation of why the drawable becoming dynamically
sized broke it and how the fix cancels the engine's fixed math regardless of true drawable
size.

Since audit D1/D4 that answer is no longer the compile-time `{0,0,1136,640}`: the point space
is derived from the window aspect and a UI-scale setting, so it is recomputed on every metrics
change (`eden_gl_set_pick_viewport()`). **The point space is not fixed for the process lifetime
any more** — nothing on the input path may cache it. `Input::scr_width/scr_height` follow it via
`Input::screenMetricsChanged()`, and the page re-reads `eden_display_point_width/height()` on
every layout pass rather than holding the old 568×320 constants. See
[ui.md](ui.md#the-point-space-derived-not-pinned-audit-rows-d1--d4).

## FOV
A `-Wl,--wrap=` on `gluPerspective` (`Classes/Graphics.mm` hard-codes 80°). Picking
still works correctly at a non-default FOV "for free," because `Util.mm`'s unproject
reads the resulting projection matrix back rather than assuming 80° anywhere.

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

Crouch is **off by default** (`CROUCH_ENABLED=false`, `Classes/Player.mm:32`) and is a port-exposed
settings toggle, not a compile-time constant like `FLY_MODE`: the "Crouch mode" row in the new
"Experiments" settings tab (see [ui.md](ui.md)) calls `eden_set_crouch_enabled()`
(`src/seam/Input_web.mm`, mirroring `eden_set_fly_mode`/`eden_get_fly_mode`), which is wired through
`eden_apply_setting()` in `src/seam/Settings_web.mm`. When it's off, `Hud.mm` also hides the crouch
button entirely — its hit-test loop and its render/draw call are both wrapped in
`if (CROUCH_ENABLED)` (`extern BOOL CROUCH_ENABLED`) — so there's no dead button sitting on screen,
matching how `FLY_MODE` already hides its own up/down affordances when off.

## Middle-click eyedropper (`eden_pick_block_at_crosshair`, `src/seam/Input_web.mm`)
Middle-click raycasts the crosshair block and copies its type into the current hotbar
slot (`Terrain::getLand`). **Fixed 2026-07-31:** it read type only and dropped the
block's paint color, unlike the real 35-cell picker (`Hud.mm`) which always sets
`hud->block_paintcolor` alongside `blocktype`. Now also reads `Terrain::getColor(x,z,y)`
and assigns it to `hud->block_paintcolor`, so eyedropping a painted block carries the
color into the next placement.

## Port-invented UI riding on this input layer
A curated 9-block hotbar strip and a real DOM crosshair are **port inventions** — the
original engine has no hotbar concept, only a scrolling block-picker grid. See
[ui.md](ui.md).
