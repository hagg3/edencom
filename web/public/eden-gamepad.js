// eden-gamepad.js — Gamepad API support for the Eden web port.
// Requires: nothing from window.Eden* (the one dependency-free leaf) — only navigator.getGamepads()
// and the bridge object eden-st.html passes at init. Publishes: window.EdenGamepad. See docs/ui.md's
// dependency graph (audit I2).
//
// Perf-audit row #24 ("Gamepad, VKeyboard, networking"): the audit's own note was that
// `Classes/Gamepad.mm` already compiles and "the Gamepad API maps cleanly onto the same
// eden_set_move_input/eden_apply_look_delta entry points". It does — and that is the whole design
// here. This file adds NO new engine exports and NO new wasm code: it is a translator that turns
// the browser's polled gamepad state into exactly the same calls the keyboard/mouse path already
// makes, via a bridge object eden-st.html hands it at init time.
//
// Why NOT Classes/Gamepad.mm: that file is the 2010 iOS-era MFi/attachment plumbing (see
// ../docs/player-input-camera.md). Wiring the browser's Gamepad API into it would be a platform
// difference living in engine code, which web/CLAUDE.md rule 2 forbids. The port's own input seam
// (src/seam/Input_web.mm) is the right target, and it is already fully exposed.
//
// Polling, not events: the Gamepad API has no per-button events at all — `navigator.getGamepads()`
// must be sampled once per frame. `tick()` is called from eden-st.html's existing rAF loop
// (trackCursorNeed), BEFORE its recomputeMove(), so the analog axes this file records are picked up
// in the same frame rather than one frame late.
//
// Settings: `gamepad` (on/off), `gamepad_look_sensitivity`, `gamepad_deadzone` are rows in
// src/seam/Settings_web.mm's kSettings[] like every other setting — per the port's "add a setting
// in C, never in JS" rule. Nothing here is persisted by this file.
(function () {
  'use strict';

  // Standard Gamepad mapping (https://w3c.github.io/gamepad/#remapping). Only honoured when
  // `pad.mapping === 'standard'`; a non-standard pad is reported (once) and otherwise ignored,
  // because guessing at an unknown layout produces a player mining at random, which is worse than
  // no gamepad support.
  var BTN = {
    A: 0, B: 1, X: 2, Y: 3,
    LB: 4, RB: 5, LT: 6, RT: 7,
    SELECT: 8, START: 9, L3: 10, R3: 11,
    DUP: 12, DDOWN: 13, DLEFT: 14, DRIGHT: 15,
  };

  // Buttons that map onto an existing keybind ACTION (the same vocabulary eden-st.html's
  // actionDown/actionUp use, so a gamepad press is indistinguishable from the equivalent key —
  // including all the momentary/continuous handling those already do).
  var BUTTON_ACTIONS = {};
  BUTTON_ACTIONS[BTN.A]      = 'jump';        // also "ascend" while flying, same as Space
  BUTTON_ACTIONS[BTN.B]      = 'crouch';
  BUTTON_ACTIONS[BTN.X]      = 'blockPicker';
  BUTTON_ACTIONS[BTN.Y]      = 'flyToggle';
  BUTTON_ACTIONS[BTN.L3]     = 'sprint';
  BUTTON_ACTIONS[BTN.R3]     = 'flyDown';     // descend while flying; harmless on foot
  BUTTON_ACTIONS[BTN.SELECT] = 'settings';
  BUTTON_ACTIONS[BTN.START]  = 'menu';
  BUTTON_ACTIONS[BTN.DUP]    = 'fireTool';
  BUTTON_ACTIONS[BTN.DDOWN]  = 'colorPicker';

  // Buttons handled specially below rather than through an action.
  //   LT / RT -> build / mine, routed through the SAME hold-to-act state machine the mouse uses
  //              (so `hold_to_act` and its repeat timing apply identically).
  //   LB / RB -> hotbar prev/next.
  //   D-pad L/R -> hotbar prev/next as well (a second, more discoverable binding).

  var TRIGGER_THRESHOLD = 0.5;  // analog triggers report .value; .pressed is driver-dependent

  // Full-tilt look speed in eden_apply_look_delta units per second. That function turns one unit
  // into 0.4deg * IS_IPAD's 2x * the player's mouse-sensitivity setting, so 225 u/s is ~180 deg/s
  // at sensitivity 1.0 — a deliberate default, roughly a console shooter's medium. The
  // `gamepad_look_sensitivity` setting scales it.
  var LOOK_RATE = 225;
  // Response curve exponent on the right stick. 1.0 is linear (twitchy near centre); 2.0 gives
  // fine control for small deflections while keeping full speed at full tilt.
  var LOOK_EXPO = 2.0;
  // Look is integrated against real elapsed time, but a long stall (tab hidden, a slow world-load
  // frame) must not snap the camera around — clamp the step.
  var MAX_LOOK_DT = 0.1;

  var bridge = null;
  var padIndex = null;          // index into navigator.getGamepads(), or null
  var prevButtons = [];         // pressed-state per button on the last tick
  var moveAxes = { forward: 0, strafe: 0, speedMul: 1 };
  var moveActive = false;       // does the left stick currently have anything to say?
  var lastLookAt = 0;
  var warnedNonStandard = false;
  var heldTrigger = null;       // BTN.LT / BTN.RT / null — which trigger owns the hold-to-act slot

  function supported() {
    return typeof navigator !== 'undefined' && typeof navigator.getGamepads === 'function';
  }

  function setting(key, fallback) {
    if (!bridge || !bridge.getSetting) return fallback;
    var v = bridge.getSetting(key);
    return (typeof v === 'number' && isFinite(v)) ? v : fallback;
  }

  function pads() {
    try { return navigator.getGamepads() || []; } catch (e) { return []; }
  }

  function activePad() {
    var list = pads();
    if (padIndex !== null) {
      var p = list[padIndex];
      if (p && p.connected) return p;
      padIndex = null;
    }
    // Adopt the first connected pad that has actually been touched, so a plugged-in-but-idle
    // controller doesn't steal input from the keyboard. (The Gamepad API deliberately hides pads
    // from getGamepads() until the user presses something on them in most browsers, but Chrome
    // exposes them immediately once one has been interacted with, so this is belt-and-braces.)
    for (var i = 0; i < list.length; i++) {
      if (list[i] && list[i].connected) { padIndex = i; return list[i]; }
    }
    return null;
  }

  // Radial deadzone with rescaling, applied to a stick as a pair: the classic per-axis deadzone
  // makes diagonals feel notched, and rescaling keeps full speed reachable at the deadzone edge.
  function applyDeadzone(x, y, dz) {
    var mag = Math.sqrt(x * x + y * y);
    if (mag <= dz) return [0, 0, 0];
    var scaled = Math.min(1, (mag - dz) / (1 - dz));
    var k = scaled / mag;
    return [x * k, y * k, scaled];
  }

  function releaseAll() {
    if (!bridge) return;
    for (var b in BUTTON_ACTIONS) {
      if (prevButtons[b]) bridge.actionUp(BUTTON_ACTIONS[b]);
    }
    if (heldTrigger !== null) { bridge.holdActStop(); heldTrigger = null; }
    prevButtons = [];
    if (moveActive) {
      moveAxes.forward = moveAxes.strafe = 0;
      moveAxes.speedMul = 1;
      moveActive = false;
      bridge.recomputeMove();
    }
  }

  function tick() {
    if (!bridge || !supported()) return;

    var enabled = setting('gamepad', 1) > 0.5;
    var pad = enabled ? activePad() : null;
    if (!pad) { if (moveActive || heldTrigger !== null) releaseAll(); lastLookAt = 0; return; }
    if (pad.mapping !== 'standard') {
      if (!warnedNonStandard) {
        warnedNonStandard = true;
        console.warn('[eden-gamepad] ignoring non-standard mapping:', pad.id);
      }
      return;
    }

    // A DOM panel (settings, pause, main menu, load-error) owns input while it is up. Release
    // anything held rather than leaving a stick latched behind the overlay — same reasoning as
    // eden-st.html's blur handler.
    if (bridge.isBlocked && bridge.isBlocked()) { releaseAll(); lastLookAt = 0; return; }

    var dz = setting('gamepad_deadzone', 0.15);
    var buttons = pad.buttons || [];
    var axes = pad.axes || [];

    function pressed(i) {
      var b = buttons[i];
      if (!b) return false;
      if (i === BTN.LT || i === BTN.RT) return (b.value || 0) > TRIGGER_THRESHOLD || !!b.pressed;
      return !!b.pressed;
    }

    // --- left stick + d-pad-free movement ---
    var lm = applyDeadzone(axes[0] || 0, axes[1] || 0, dz);
    var strafe = lm[0];
    var forward = -lm[1];           // stick Y is +down
    var nowActive = (strafe !== 0 || forward !== 0);
    var speedMul = pressed(BTN.L3) ? 1.3 : 1.0;
    if (nowActive || moveActive) {
      moveAxes.forward = forward;
      moveAxes.strafe = strafe;
      moveAxes.speedMul = speedMul;
      moveActive = nowActive;
      // Deliberately NOT calling eden_set_move_input here. eden-st.html's recomputeMove() runs
      // unconditionally every frame and would immediately overwrite it with the keyboard-only
      // axes; it reads axes() below instead, so the two sources compose in exactly one place —
      // and since tick() is called from that same loop just BEFORE recomputeMove(), the stick is
      // picked up in the same frame with no extra wasm call. (releaseAll() does call it, because
      // it also fires from the gamepaddisconnected event, outside the loop.)
    }

    // --- right stick look ---
    var now = performance.now();
    var dt = lastLookAt ? Math.min((now - lastLookAt) / 1000, MAX_LOOK_DT) : 0;
    lastLookAt = now;
    var rm = applyDeadzone(axes[2] || 0, axes[3] || 0, dz);
    if (dt > 0 && (rm[0] !== 0 || rm[1] !== 0)) {
      var curve = Math.pow(rm[2], LOOK_EXPO) / (rm[2] || 1);  // scale magnitude, keep direction
      var rate = LOOK_RATE * setting('gamepad_look_sensitivity', 1) * dt * curve;
      bridge.applyLook(rm[0] * rate, rm[1] * rate);
    }

    // --- action buttons (edge-triggered, mapped onto keybind actions) ---
    for (var key in BUTTON_ACTIONS) {
      var i = +key;
      var down = pressed(i);
      if (down !== !!prevButtons[i]) {
        prevButtons[i] = down;
        if (down) bridge.actionDown(BUTTON_ACTIONS[i]);
        else bridge.actionUp(BUTTON_ACTIONS[i]);
      }
    }

    // --- triggers -> mine/build through the mouse's hold-to-act machine ---
    // Only one at a time: holdActStart() ignores a second press while one is held, so tracking
    // which trigger owns the slot is what makes the matching Stop fire for the right one.
    var rt = pressed(BTN.RT), lt = pressed(BTN.LT);
    if (heldTrigger === BTN.RT && !rt) { bridge.holdActStop(); heldTrigger = null; }
    if (heldTrigger === BTN.LT && !lt) { bridge.holdActStop(); heldTrigger = null; }
    if (heldTrigger === null) {
      if (rt) { heldTrigger = BTN.RT; bridge.holdActStart(false); }       // mine
      else if (lt) { heldTrigger = BTN.LT; bridge.holdActStart(true); }   // build
    }

    // --- hotbar (shoulder buttons and d-pad left/right, both edge-triggered) ---
    var scroll = 0;
    if (pressed(BTN.RB) && !prevButtons[BTN.RB]) scroll += 1;
    if (pressed(BTN.LB) && !prevButtons[BTN.LB]) scroll -= 1;
    if (pressed(BTN.DRIGHT) && !prevButtons[BTN.DRIGHT]) scroll += 1;
    if (pressed(BTN.DLEFT) && !prevButtons[BTN.DLEFT]) scroll -= 1;
    prevButtons[BTN.RB] = pressed(BTN.RB);
    prevButtons[BTN.LB] = pressed(BTN.LB);
    prevButtons[BTN.DRIGHT] = pressed(BTN.DRIGHT);
    prevButtons[BTN.DLEFT] = pressed(BTN.DLEFT);
    if (scroll) bridge.hotbarScroll(scroll);
  }

  window.EdenGamepad = {
    // bridge: { actionDown, actionUp, recomputeMove, applyLook, holdActStart, holdActStop,
    //           hotbarScroll, isBlocked, getSetting, onConnect }
    init: function (b) {
      bridge = b;
      if (!supported()) return;
      window.addEventListener('gamepadconnected', function (e) {
        if (padIndex === null && e.gamepad) padIndex = e.gamepad.index;
        if (bridge && bridge.onConnect) bridge.onConnect(e.gamepad ? e.gamepad.id : '', true);
      });
      window.addEventListener('gamepaddisconnected', function (e) {
        if (e.gamepad && e.gamepad.index === padIndex) { releaseAll(); padIndex = null; }
        if (bridge && bridge.onConnect) bridge.onConnect(e.gamepad ? e.gamepad.id : '', false);
      });
    },
    tick: tick,
    // Read by eden-st.html's recomputeMove() so keyboard and stick compose in one place.
    axes: function () { return moveActive ? moveAxes : null; },
    connected: function () { return padIndex !== null; },
    supported: supported,
  };
})();
