// eden-input.js — audit row 28/C5 split: every DOM event (touch, mouse, keyboard, wheel, gamepad)
// that turns into an engine input call, plus pointer-lock acquisition/retry and the hold-to-act
// state machine. Consumes eden-keybinds.js's codeToActions/MOMENTARY_ACTIONS/CONTINUOUS_ACTIONS
// (what a key means) and eden-viewport.js's toEnginePoint/fullscreenBtn (where a point lands /
// the fullscreen toggle's DOM handle), but owns all of the DISPATCH — deciding what an event DOES.
//
// Depends on (must load after): eden-host.js (canvasEl, moduleReady, callIfReady, jumpPulsePending,
// showToast), eden-viewport.js (toEnginePoint, fullscreenBtn), eden-keybinds.js (codeToActions,
// MOMENTARY_ACTIONS, CONTINUOUS_ACTIONS), and the settings/pausemenu/loaderror/menu/gamepad
// eden-*.js files (window.EdenSettings/EdenPauseMenu/EdenLoadError/EdenMenu/EdenGamepad), all of
// which already load earlier in eden-st.html. Declares `pointerLocked`, `holdActTick`,
// `recomputeMove`/`recomputeFly`/`recomputeCrouch` and `sendTouch` for eden-st.html's
// trackCursorNeed to call once per frame.
'use strict';

// --- Stage P3: JS pointer/touch events -> Input::getInput()'s 5-slot touch tracker ----------
// The wasm side (src/seam/Input_web.mm) exports `eden_input_pointer_event(phase, identity, x, y)`
// (EMSCRIPTEN_KEEPALIVE — no ccall/cwrap needed, this is a plain-numbers wasm export, so
// `Module._eden_input_pointer_event` exists as soon as the module has linked). This side's only
// job is DOM event -> (phase, identity, x, y) in the engine's POINT space; Input_web.mm/
// Classes/Input.mm own everything past that (Y-flip, SCALE_* on other device profiles, etc.) —
// see that file's header comment for the coordinate contract.
const PHASE_START = 0, PHASE_MOVE = 1, PHASE_END = 2, PHASE_CANCEL = 3;
const MOUSE_IDENTITY = -1; // sentinel distinct from any real Touch.identifier (always >= 0)

function sendTouch(phase, identity, clientX, clientY) {
  if (!moduleReady) return;
  const p = toEnginePoint(clientX, clientY);
  Module._eden_input_pointer_event(phase, identity, p.x, p.y);
}

// pointerLocked is declared here but assigned by the 'pointerlockchange' listener further down in
// this same file (event callbacks only run after the whole document's scripts have executed once)
// — guard here so a locked pointer's frozen clientX/clientY (pointer lock stops normal cursor
// movement/coords) doesn't ALSO synthesize a stale-position touch alongside the Pass 23 look/click
// path below. A picker/menu selection made through these listeners can close the picker as a side
// effect of the very click that made it (e.g. tapping a swatch) — reacquireLockIfJustClosed()
// re-requests pointer lock synchronously, still inside this real DOM event's call stack, so the
// browser still credits it as gesture-backed. trackCursorNeed's own reacquire (in eden-st.html's
// per-frame poll) fires a tick later from a non-gesture rAF context and is liable to be silently
// rejected — this is what used to cost the player one extra "regain focus" click after every
// picker use.
//
// Second bug on top of the first (still reproduced after the above landed): this must NOT check
// Module._eden_ui_wants_cursor() to decide whether the picker just closed. sendTouch() above only
// enqueues the touch into Input::getInput()'s slot table (Input_web.mm's eden_input_pointer_event
// calls touchesBegan/Ended synchronously) — but the thing that actually reads a picker-swatch touch
// and flips hud->mode back out of MODE_PICK_BLOCK/MODE_PICK_COLOR is Hud::update() (Classes/Hud.mm),
// which only runs on the ENGINE's next tick (World::update(), driven by emscripten_set_main_loop's
// rAF). So eden_ui_wants_cursor(), called synchronously right after sendTouch() in this same
// handler, still reports the picker as open on the exact click that closes it — the guard below
// used to bail out on that stale read every time, silently falling back to trackCursorNeed's
// non-gesture reacquire (and the extra click). Trust wantedCursorBefore (an accurate READ from
// before this gesture, past the engine's last tick) instead: if a picker was open, attempt the
// relock unconditionally. If the click didn't actually close it (e.g. it missed every swatch),
// trackCursorNeed's per-frame poll re-exits the lock next frame — a one-frame flicker, not a stuck
// cursor. The JS-owned overlays (settings/pause/load-error/main menu) are still checked here
// because their isOpen() is plain synchronous JS state, not engine-tick-delayed, so it's accurate
// to read immediately.
function reacquireLockIfJustClosed(wantedCursorBefore) {
  if (!moduleReady || !wantedCursorBefore) return;
  if (window.EdenSettings.isOpen() || window.EdenPauseMenu.isOpen() ||
      window.EdenLoadError.isOpen() || window.EdenMenu.isOpen()) return;
  if (document.pointerLockElement === canvasEl) return;
  wantLock();
}
// --- Audit row 17/G1: touch-draggable controls card (the on-screen joystick pad) ------------
// A deliberately separate, EXPLICIT mode (entered via the pause menu's "Move controls" button,
// eden-pausemenu.js — touch profile only) rather than an ambiguous long-press during normal play:
// Classes/Joystick.mm's joystickCustomizeMode flag makes Joystick::update no-op entirely while
// this is on (no movement input, no risk of a drag fighting real gameplay touch handling), so all
// this needs to do is translate the drag gesture into eden_joystick_set_origin() calls and
// persist the result. Wired into both the touch AND (unlocked) mouse paths below — the mouse path
// costs nothing extra and makes this actually testable on a desktop browser without a phone.
const JOYSTICK_POS_STORAGE_KEY = 'eden.prefs.joystick_pos';
let joystickMoveModeOn = false;
let joystickDragId = null;               // the touch/mouse identity currently dragging, or null
let joystickDragOffsetX = 0, joystickDragOffsetY = 0; // finger pos minus the pad's origin, at drag start

function clampJoystickOrigin(x, y) {
  const size = moduleReady ? Module._eden_joystick_get_pad_size() : 88;
  return {
    x: Math.max(0, Math.min(ENGINE_WIDTH - size, x)),
    y: Math.max(0, Math.min(ENGINE_HEIGHT - size, y)),
  };
}
function persistJoystickPos() {
  callIfReady(() => {
    const x = Module._eden_joystick_get_origin_x(), y = Module._eden_joystick_get_origin_y();
    try { localStorage.setItem(JOYSTICK_POS_STORAGE_KEY, JSON.stringify({ x, y })); } catch (e) {}
  });
}
function joystickMoveBegin(identity, clientX, clientY) {
  if (joystickDragId !== null || !moduleReady) return; // already dragging with another finger
  joystickDragId = identity;
  const p = toEnginePoint(clientX, clientY);
  joystickDragOffsetX = p.x - Module._eden_joystick_get_origin_x();
  joystickDragOffsetY = p.y - Module._eden_joystick_get_origin_y();
}
function joystickMoveDrag(identity, clientX, clientY) {
  if (joystickDragId !== identity || !moduleReady) return;
  const p = toEnginePoint(clientX, clientY);
  const c = clampJoystickOrigin(p.x - joystickDragOffsetX, p.y - joystickDragOffsetY);
  Module._eden_joystick_set_origin(c.x, c.y);
}
function joystickMoveEnd(identity) {
  if (joystickDragId !== identity) return;
  joystickDragId = null;
  persistJoystickPos();
}
// Restores a customized position on boot — mirrors eden-hotbar.js's restoreHotbar() pattern
// (called once eden_settings_loaded(), from eden-st.html's trackCursorNeed, alongside the other
// restore-on-first-available-frame calls).
function restoreJoystickPos() {
  let saved = null;
  try { saved = JSON.parse(localStorage.getItem(JOYSTICK_POS_STORAGE_KEY) || 'null'); } catch (e) {}
  if (!saved || typeof saved.x !== 'number' || typeof saved.y !== 'number') return;
  callIfReady(() => {
    const c = clampJoystickOrigin(saved.x, saved.y);
    Module._eden_joystick_set_origin(c.x, c.y);
  });
}
window.EdenJoystickCustomize = {
  start: function () {
    joystickMoveModeOn = true;
    joystickDragId = null;
    callIfReady(() => Module._eden_joystick_set_customize_mode(1));
  },
  stop: function () {
    joystickMoveModeOn = false;
    joystickDragId = null;
    callIfReady(() => Module._eden_joystick_set_customize_mode(0));
  },
  isActive: function () { return joystickMoveModeOn; },
  restoreOnBoot: restoreJoystickPos,
  resetToDefault: function () {
    try { localStorage.removeItem(JOYSTICK_POS_STORAGE_KEY); } catch (e) {}
    // Mirrors Joystick.mm's own constructor default (padbounds origin 20,20) — there is no
    // exported getter for that default, only the live (possibly customized) origin.
    callIfReady(() => Module._eden_joystick_set_origin(20, 20));
  },
};

let mouseDown = false;
canvasEl.addEventListener('mousedown', (e) => {
  if (pointerLocked) return;
  if (joystickMoveModeOn) { joystickMoveBegin(MOUSE_IDENTITY, e.clientX, e.clientY); return; }
  mouseDown = true;
  const wantedCursorBefore = moduleReady && !!Module._eden_ui_wants_cursor();
  sendTouch(PHASE_START, MOUSE_IDENTITY, e.clientX, e.clientY);
  reacquireLockIfJustClosed(wantedCursorBefore);
});
canvasEl.addEventListener('mousemove', (e) => {
  if (joystickMoveModeOn) { joystickMoveDrag(MOUSE_IDENTITY, e.clientX, e.clientY); return; }
  if (pointerLocked || !mouseDown) return;
  sendTouch(PHASE_MOVE, MOUSE_IDENTITY, e.clientX, e.clientY);
});
window.addEventListener('mouseup', (e) => {
  if (joystickMoveModeOn) { joystickMoveEnd(MOUSE_IDENTITY); return; }
  if (pointerLocked || !mouseDown) return;
  mouseDown = false;
  const wantedCursorBefore = moduleReady && !!Module._eden_ui_wants_cursor();
  sendTouch(PHASE_END, MOUSE_IDENTITY, e.clientX, e.clientY);
  reacquireLockIfJustClosed(wantedCursorBefore);
});
// A pointer leaving the window entirely mid-drag (not just the canvas) is the closest DOM analogue
// to iOS's -touchesCancelled: — deliver the same signal rather than leaving Input.mm's touch slot
// stuck "down" forever.
window.addEventListener('blur', () => {
  if (!mouseDown) return;
  mouseDown = false;
  if (moduleReady) Module._eden_input_pointer_event(PHASE_CANCEL, MOUSE_IDENTITY, 0, 0);
});

function forEachChangedTouch(e, fn) {
  for (let i = 0; i < e.changedTouches.length; i++) {
    const t = e.changedTouches[i];
    fn(t.identifier, t.clientX, t.clientY);
  }
}
// Phase 2 (input mode audit): a real touch/key/pointer event is unambiguous evidence of which
// profile is actually in use, overriding the pre-interaction matchMedia guess. No-ops on the C
// side while `input_mode` is not Auto (eden_effective_input_is_touch() ignores this input once the
// player has picked one explicitly) — see Settings_web.mm.
//
// Audit item Q4 (C6 "input-profile flapping"): this used to be a bare `mousemove` listener that
// called into wasm on EVERY move, and on a hybrid device (touchscreen laptop, iPad+trackpad,
// Android+mouse) the synthetic mouse event that follows a real touch would immediately flip the
// profile back to desktop mid-touch-session, killing the on-screen joystick under the player's
// thumb. Fix: key off `PointerEvent.pointerType`, which is unambiguous ('touch' vs 'mouse'/'pen' —
// a synthetic mouse event fired after a touch is still reported as pointerType 'touch'), and only
// call into wasm when the detected profile actually changes, not on every event — this also kills
// the wasm call on every mousemove.
let lastDetectedTouch = null;
function noteDetectedTouch(isTouch) {
  if (lastDetectedTouch === isTouch) return;
  lastDetectedTouch = isTouch;
  callIfReady(() => {
    Module._eden_set_detected_touch(isTouch ? 1 : 0);
    // Audit D4: the detected profile also picks the `ui_scale`/`display_layout` defaults while
    // those rows sit on Auto, so a profile flip is a point-space change. The C side has already
    // re-derived and re-laid-out by now; this re-letterboxes the canvas to the new engine aspect.
    applyDisplayMode();
  });
}
canvasEl.addEventListener('touchstart', () => {
  noteDetectedTouch(true);
}, { passive: true });
window.addEventListener('keydown', () => {
  noteDetectedTouch(false);
});
// 'pen' is treated as mouse-like (precise pointer, not a touch surface) per the audit's own
// wording. pointerType is why this is safe against the hybrid-device flapping bug above.
window.addEventListener('pointerdown', (e) => {
  noteDetectedTouch(e.pointerType === 'touch');
});
window.addEventListener('pointermove', (e) => {
  noteDetectedTouch(e.pointerType === 'touch');
});

canvasEl.addEventListener('touchstart', (e) => {
  e.preventDefault();
  if (joystickMoveModeOn) {
    const t = e.changedTouches[0];
    if (t) joystickMoveBegin(t.identifier, t.clientX, t.clientY);
    return;
  }
  forEachChangedTouch(e, (id, x, y) => sendTouch(PHASE_START, id, x, y));
}, { passive: false });
canvasEl.addEventListener('touchmove', (e) => {
  e.preventDefault();
  if (joystickMoveModeOn) {
    forEachChangedTouch(e, (id, x, y) => joystickMoveDrag(id, x, y));
    return;
  }
  forEachChangedTouch(e, (id, x, y) => sendTouch(PHASE_MOVE, id, x, y));
}, { passive: false });
canvasEl.addEventListener('touchend', (e) => {
  e.preventDefault();
  if (joystickMoveModeOn) {
    for (let i = 0; i < e.changedTouches.length; i++) joystickMoveEnd(e.changedTouches[i].identifier);
    return;
  }
  forEachChangedTouch(e, (id, x, y) => sendTouch(PHASE_END, id, x, y));
}, { passive: false });
canvasEl.addEventListener('touchcancel', (e) => {
  e.preventDefault();
  if (joystickMoveModeOn) {
    for (let i = 0; i < e.changedTouches.length; i++) joystickMoveEnd(e.changedTouches[i].identifier);
    return;
  }
  forEachChangedTouch(e, (id, x, y) => sendTouch(PHASE_CANCEL, id, x, y));
}, { passive: false });

// --- Pass 23: desktop keyboard/mouse controls -----------------------------------------------
// WASD move / Shift sprint / Alt walk / mouse-look / left-click mine / right-click build /
// wheel+1-9 hotbar / E blocks / C colors / ESC menu / pointer-lock on canvas click. The C++ side of
// all of this is web/src/seam/Input_web.mm (see its "Pass 23" comment block) — this is only DOM
// event -> those exported functions, same division of labor as the Stage P3 touch code above.
// Movement/look/fly-thrust bypass the touch tracker entirely (Input_web.mm explains why); click
// and E/C/ESC reuse it (a synthesized tap needs >=1 engine tick between begin and end, see that
// file — real DOM mousedown/up and keydown/up naturally straddle a frame, so no extra care is
// needed here, unlike the headless driver which has to tick explicitly).

// Plain pointer lock. F9g originally requested `{unadjustedMovement:true}` here (raw,
// OS-acceleration-free mouse movement) — REVERTED after a live playtest reported look sensitivity
// as noticeably higher than before: disabling the OS's own pointer-acceleration curve changes the
// effective deltas by an amount that depends on the player's own mouse/OS settings, which is a
// worse default than "matches what every other control on the page already did." If raw input is
// wanted again, it belongs behind its own opt-in setting (with its own sensitivity recalibration),
// not as a silent default.
function requestLock() {
  try { canvasEl.requestPointerLock(); } catch (err) {}
}

// PC-controls bug: Chromium enforces a real ~1.2s cooldown on requestPointerLock() shortly after
// any pointer-lock exit (e.g. the Escape that closes a menu/picker), and silently rejects calls
// made inside it — no exception (the try/catch above never sees it), just a 'pointerlockerror'
// event. Both call sites below used to be one-shot: a request made inside that window just failed
// and nothing retried it, so the player had to keep clicking blind and racing the cooldown, which
// is exactly the "randomly takes 3-6 clicks to mine/build" symptom (root-caused 2026-07-30, no
// separate bug in Player/Hud's touch consumption). Fix: track "a lock is wanted" and retry on
// 'pointerlockerror' with a short backoff until it succeeds, the cooldown clears, or the player
// stops wanting one (a picker/menu opened).
let lockWanted = false;
let lockRetryTimer = null;
let lockRetryCount = 0;
const LOCK_RETRY_MS = 250;
const LOCK_RETRY_MAX = 6;   // 6 * 250ms comfortably covers the ~1.2s cooldown
function wantLock() {
  lockWanted = true;
  lockRetryCount = 0;
  requestLock();
}
function stopWantingLock() {
  lockWanted = false;
  if (lockRetryTimer) { clearTimeout(lockRetryTimer); lockRetryTimer = null; }
}
document.addEventListener('pointerlockerror', () => {
  if (!lockWanted || lockRetryTimer || lockRetryCount >= LOCK_RETRY_MAX) return;
  lockRetryCount++;
  lockRetryTimer = setTimeout(() => {
    lockRetryTimer = null;
    if (lockWanted && document.pointerLockElement !== canvasEl) requestLock();
  }, LOCK_RETRY_MS);
});

// requestPointerLock is the "unlock the cursor for click-to-place/break like touch" trigger the
// spec asks for — while locked, mousemove reports relative deltas (movementX/Y) instead of an
// absolute position, which is what turns the mouse into a look control instead of a drag-to-look
// pointer. Only engage it on a plain left click with no modifier (avoids hijacking e.g. a
// ctrl-click browser context-menu gesture) and only once the module/world is up.
// ...except while a screen that you POINT at is open. The block and colour pickers (and the menu)
// are grids of swatches: with the pointer locked the cursor is hidden and mousemove is consumed as
// look input, so they cannot be used at all. Minecraft's convention is the one players expect —
// opening the inventory releases the mouse, closing it re-grabs — so the lock follows
// eden_ui_wants_cursor() rather than being purely click-driven.
canvasEl.addEventListener('click', () => {
  if (document.pointerLockElement === canvasEl) return;
  callIfReady(() => { if (!Module._eden_ui_wants_cursor()) wantLock(); });
});

// --- PC controls audit F4: hold-to-mine/build state machine --------------------------------
// Driven from the rAF loop (holdActTick, called once per frame from eden-st.html's
// trackCursorNeed), NOT setInterval — eden_click_begin/end must straddle at least one engine tick
// (Input_web.mm's tap-mine/build fires on M_DOWN then M_RELEASE), which only a frame-driven state
// machine guarantees. Per held button: fire immediately on mousedown, then after an initial ~250ms
// delay repeat on a ~200ms period, each repeat spending one frame in "begin" and the next in "end".
// Falls back to plain click-once behaviour when the `hold_to_act` setting is off
// (Module._eden_get_hold_to_act() reads it).
const HOLD_INITIAL_DELAY = 250, HOLD_REPEAT_PERIOD = 200;
let holdAction = null; // {isBuild, pendingEnd, firstFireDone, nextFireAt}
function holdActStart(isBuild) {
  if (holdAction) return; // a second button pressed while one is already held: ignore it
  holdAction = { isBuild, pendingEnd: false, firstFireDone: false, nextFireAt: 0 };
}
function holdActStop() {
  if (!holdAction) return;
  if (holdAction.pendingEnd) callIfReady(() => Module._eden_click_end(holdAction.isBuild ? 1 : 0));
  holdAction = null;
}
function holdActTick() {
  if (!holdAction || !moduleReady) return;
  const now = performance.now();
  if (holdAction.pendingEnd) {
    Module._eden_click_end(holdAction.isBuild ? 1 : 0);
    holdAction.pendingEnd = false;
    const wasFirst = !holdAction.firstFireDone;
    holdAction.firstFireDone = true;
    holdAction.nextFireAt = Module._eden_get_hold_to_act()
      ? now + (wasFirst ? HOLD_INITIAL_DELAY : HOLD_REPEAT_PERIOD)
      : Infinity; // hold-to-act off: one shot per press, exactly like a plain click
    return;
  }
  if (!holdAction.firstFireDone || now >= holdAction.nextFireAt) {
    Module._eden_click_begin(holdAction.isBuild ? 1 : 0);
    holdAction.pendingEnd = true;
  }
}

// Row #24: hand the gamepad translator the same entry points the keyboard/mouse path uses, so a
// controller press is literally indistinguishable from the equivalent key (including the
// momentary/continuous and hold-to-act handling those already implement). See eden-gamepad.js.
window.EdenGamepad.init({
  actionDown, actionUp,
  recomputeMove,
  applyLook: (dx, dy) => callIfReady(() => Module._eden_apply_look_delta(dx, dy)),
  holdActStart, holdActStop,
  hotbarScroll: (dir) => callIfReady(() => Module._eden_hotbar_scroll(dir)),
  // A DOM panel owns input while it is up — same set the pointer-lock arbitration below uses.
  isBlocked: () => window.EdenSettings.isOpen() || window.EdenPauseMenu.isOpen() ||
    window.EdenLoadError.isOpen() || window.EdenMenu.isOpen(),
  getSetting: (key) => window.EdenSettings.getByKey(key),
  onConnect: (id, connected) =>
    showToast(connected ? ('Gamepad connected' + (id ? ': ' + id.split('(')[0].trim() : ''))
                        : 'Gamepad disconnected'),
});

const actionsDown = new Set();
function isDown(action) { return actionsDown.has(action); }

function recomputeMove() {
  // eden_set_move_input's comment explains why raw -1..1 axes, not per-key calls, are what the
  // engine side wants.
  let forward = (isDown('moveForward') ? 1 : 0) - (isDown('moveBack') ? 1 : 0);
  let strafe = (isDown('moveRight') ? 1 : 0) - (isDown('moveLeft') ? 1 : 0);
  let speedMul = isDown('sprint') ? 1.3 : (isDown('walk') ? 0.5 : 1.0);
  // Row #24: the gamepad's left stick composes here rather than calling _eden_set_move_input
  // itself, because this function runs unconditionally every frame (the F1 frame-rate-normalize
  // fix requires it) and would otherwise stomp the stick one frame later. Sum-and-clamp rather than
  // "stick wins": holding W while nudging the stick should not suddenly stop the player, and the
  // clamp keeps the -1..1 contract eden_set_move_input wants.
  const pad = window.EdenGamepad && window.EdenGamepad.axes();
  if (pad) {
    forward = Math.max(-1, Math.min(1, forward + pad.forward));
    strafe = Math.max(-1, Math.min(1, strafe + pad.strafe));
    speedMul = Math.max(speedMul, pad.speedMul);
  }
  callIfReady(() => Module._eden_set_move_input(forward, strafe, speedMul));
}
function recomputeFly() {
  callIfReady(() => Module._eden_set_fly_thrust(isDown('jump') ? 1 : 0, isDown('flyDown') ? 1 : 0));
}
function recomputeCrouch() {
  callIfReady(() => Module._eden_set_crouch(isDown('crouch') ? 1 : 0));
}

function actionDown(action) {
  if (CONTINUOUS_ACTIONS.has(action)) {
    actionsDown.add(action);
    if (action === 'flyDown' || action === 'jump') recomputeFly();
    if (action === 'crouch') recomputeCrouch();
    if (action !== 'flyDown' && action !== 'crouch') recomputeMove(); // 'jump' affects both fly thrust and nothing in recomputeMove; harmless extra call
  }
  switch (action) {
    case 'jump':
      callIfReady(() => Module._eden_set_jump(1)); break;
    case 'blockPicker':
      callIfReady(() => Module._eden_tap_hud_button_begin(1)); break;
    case 'colorPicker':
      callIfReady(() => Module._eden_tap_hud_button_begin(2)); break;
    case 'fireTool':
      callIfReady(() => Module._eden_tap_hud_button_begin(7)); break;
    case 'menu':
      // Panel first: while it is up, the menu action closes the panel, not the engine's own
      // in-game menu underneath it. F9b: while pointer-locked, the browser force-exits lock on
      // Escape and this cannot be prevented — so locked, this means ONLY "release the mouse", not
      // "also open the pause menu". Only tap the menu button when already unlocked. (This F9b
      // nuance is specific to Escape's browser behaviour, so it still applies even if the player
      // rebinds the "menu" action to a different key — the pointer-lock release itself is only
      // ever triggered by the real Escape key regardless of this binding.)
      if (window.EdenSettings.isOpen()) { window.EdenSettings.close(); break; }
      if (pointerLocked) break;
      callIfReady(() => Module._eden_tap_hud_button_begin(0)); break;
    case 'blockPreview':
      // Routed through the settings model so the key and the panel share one piece of state.
      callIfReady(() => {
        const on = window.EdenSettings.toggleByKey('block_preview');
        showToast('Block preview ' + (on ? 'ON' : 'OFF'));
      });
      break;
    case 'flyToggle':
      // Fly mode toggle. The engine hard-codes it ON with no UI switch (Classes/Player.mm:32, and
      // the HUD's own toggle is commented out at Hud.mm:570), so the port owns the setting.
      callIfReady(() => {
        const on = window.EdenSettings.toggleByKey('fly');
        showToast('Fly mode ' + (on ? 'ON' : 'OFF'));
      });
      break;
    case 'settings':
      callIfReady(() => window.EdenSettings.open()); break;
    case 'fullscreen':
      fullscreenBtn.onclick(); break;
    default:
      if (action.indexOf('hotbar') === 0) {
        const slot = parseInt(action.slice(6), 10) - 1;
        callIfReady(() => Module._eden_select_hotbar_slot(slot));
      }
  }
}
function actionUp(action) {
  if (CONTINUOUS_ACTIONS.has(action)) {
    actionsDown.delete(action);
    if (action === 'flyDown' || action === 'jump') recomputeFly();
    if (action === 'crouch') recomputeCrouch();
    if (action !== 'flyDown' && action !== 'crouch') recomputeMove();
  }
  switch (action) {
    case 'jump':
      callIfReady(() => Module._eden_set_jump(0)); break;
    case 'blockPicker':
      callIfReady(() => Module._eden_tap_hud_button_end(1)); break;
    case 'colorPicker':
      callIfReady(() => Module._eden_tap_hud_button_end(2)); break;
    case 'fireTool':
      callIfReady(() => Module._eden_tap_hud_button_end(7)); break;
    case 'menu':
      callIfReady(() => Module._eden_tap_hud_button_end(0)); break;
  }
}

// A focused text field (e.g. the New World name input) must get its own keystrokes — without this,
// typing "Space" or any letter bound to an action (W/A/S/D, E, ...) both moves/jumps the player AND
// (for Space/other action keys with preventDefault) never reaches the field at all.
function isTypingTarget(el) {
  if (!el) return false;
  if (el.isContentEditable) return true;
  const tag = el.tagName;
  return tag === 'INPUT' || tag === 'TEXTAREA';
}
window.addEventListener('keydown', (e) => {
  if (isTypingTarget(document.activeElement)) return;
  const actions = codeToActions(e.code);
  if (!actions.length) return;
  // Space must not scroll the page; Alt alone must not blur it. Keyed off the ACTION (not the
  // physical code) so this still holds after a player rebinds jump/walk to a different key.
  if (actions.indexOf('jump') !== -1 || actions.indexOf('walk') !== -1) e.preventDefault();
  for (const action of actions) {
    if (e.repeat && MOMENTARY_ACTIONS.has(action)) continue;
    actionDown(action);
  }
});
window.addEventListener('keyup', (e) => {
  if (isTypingTarget(document.activeElement)) return;
  for (const action of codeToActions(e.code)) actionUp(action);
});
// Releasing focus (alt-tab, clicking outside) mid-keypress must not leave a movement/fly key
// latched down forever — there is no guaranteed keyup for that. F9a: also force-end a held
// mine/build (holdActStop composes into eden_click_end itself if one was pending) — otherwise
// alt-tabbing away mid-hold leaves that touch slot stuck down, same class of bug as the mouse one
// this shares a blur handler with.
//
// The jump key needs its OWN explicit release, not just the recompute calls below: `jump` is a
// synthetic TOUCH (Input_web.mm's eden_set_jump, held on hud->rjumphit for as long as it's "down"),
// separate from the actionsDown bookkeeping recomputeMove/Fly read — clearing actionsDown alone
// does not tell the engine to release that touch, so a held jump surviving a lost-focus moment
// (alt-tab, or a modal grabbing focus) would otherwise leave the player jumping/flying forever with
// no key actually held anymore.
window.addEventListener('blur', () => {
  const wasJumping = isDown('jump');
  actionsDown.clear();
  recomputeMove(); recomputeFly(); recomputeCrouch();
  if (wasJumping) callIfReady(() => Module._eden_set_jump(0));
  holdActStop();
});

// Phase 6: wheel-up/-down as jump (the Source bhop convention), while `advanced_movement` is on.
// eden_set_jump is a synthetic touch (Input_web.mm) whose begin/end must straddle >=1 engine tick,
// same rule as click/HUD taps — so this only sets the "begin" here and lets the per-frame
// `trackCursorNeed` call deliver "end" one frame later, never a same-call pulse.
function wheelJumpPulse() {
  callIfReady(() => Module._eden_set_jump(1));
  jumpPulsePending = true;
}

// F9c: accumulate the wheel's actual magnitude rather than treating every event as one step —
// trackpads and high-resolution mice emit many small deltaY events per physical notch, which used
// to race the hotbar (one step per DOM event, not per intended scroll click). Also normalizes
// `deltaMode` (0=pixel, 1=line, 2=page) to a common pixel-equivalent scale first.
let wheelAccum = 0;
const WHEEL_STEP_PX = 50;
canvasEl.addEventListener('wheel', (e) => {
  e.preventDefault();
  let px = e.deltaY;
  if (e.deltaMode === 1) px *= 16;                          // DOM_DELTA_LINE
  else if (e.deltaMode === 2) px *= (window.innerHeight || 800); // DOM_DELTA_PAGE
  wheelAccum += px;
  const advanced = moduleReady && !!Module._eden_get_advanced_movement();
  while (wheelAccum >= WHEEL_STEP_PX) {
    advanced ? wheelJumpPulse() : callIfReady(() => Module._eden_hotbar_scroll(1));
    wheelAccum -= WHEEL_STEP_PX;
  }
  while (wheelAccum <= -WHEEL_STEP_PX) {
    advanced ? wheelJumpPulse() : callIfReady(() => Module._eden_hotbar_scroll(-1));
    wheelAccum += WHEEL_STEP_PX;
  }
}, { passive: false });

let pointerLocked = false;
document.addEventListener('pointerlockchange', () => {
  pointerLocked = (document.pointerLockElement === canvasEl);
  if (pointerLocked) stopWantingLock();
});

// 2026-08-01: the raw 1:1 mapping from movementX/Y to eden_apply_look_delta made the "Mouse
// sensitivity" slider's default (1.00) feel too fast — this trims the mouse path only (NOT
// eden_apply_look_delta itself, which the gamepad's right-stick look also calls through
// EdenGamepad's applyLook bridge above, calibrated separately against that function's untrimmed
// units; scaling there would silently detune the gamepad too). 0.4 was picked so the slider's
// default of 1.00 now feels like the old 0.40 did; the slider's own range/steps are unchanged.
const MOUSE_SENSITIVITY_TRIM = 0.4;
canvasEl.addEventListener('mousemove', (e) => {
  if (!pointerLocked) return;
  callIfReady(() => Module._eden_apply_look_delta(
    e.movementX * MOUSE_SENSITIVITY_TRIM, e.movementY * MOUSE_SENSITIVITY_TRIM));
});
// Only while locked do mousedown/up mean mine/build-at-crosshair (routed through the F4
// hold-to-act state machine above, not a direct eden_click_begin/end pulse); when NOT locked, the
// existing Stage P3 mousedown/move/up block above still owns the mouse (that's how you click
// menu/HUD buttons before ever locking the pointer).
canvasEl.addEventListener('mousedown', (e) => {
  if (!pointerLocked) return;
  e.preventDefault();
  // Middle-click "pick block" (audit row 31/F3) — not a hold-to-act button, just a one-shot read
  // of whatever's under the crosshair into the current hotbar slot. preventDefault above already
  // suppresses the browser's native middle-click autoscroll cursor.
  if (e.button === 1) { callIfReady(() => Module._eden_pick_block_at_crosshair()); return; }
  callIfReady(() => holdActStart(e.button === 2 ? 1 : 0));
});
window.addEventListener('mouseup', (e) => {
  if (!pointerLocked) return;
  callIfReady(() => holdActStop());
});
// Right-click while locked is "build", not the browser context menu.
canvasEl.addEventListener('contextmenu', (e) => { if (pointerLocked) e.preventDefault(); });
