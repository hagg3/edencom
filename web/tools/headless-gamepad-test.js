#!/usr/bin/env node
// headless-gamepad-test.js — regression test for public/eden-gamepad.js (perf-audit row #24).
//
// The Gamepad API cannot be driven from a headless runner and a real controller cannot be plugged
// into this environment, so what IS testable — and what this covers — is the whole translator:
// deadzone shaping, stick->axes composition, edge-triggered buttons, the trigger/hold-to-act
// ownership rule, and the release-everything paths. It loads the REAL public/eden-gamepad.js into
// a sandbox with a fake `navigator.getGamepads()` and a recording bridge, so it tests the shipped
// file, not a copy of its logic.
//
// What it deliberately does NOT cover: that a real pad reports `mapping === 'standard'` with this
// button order (that is the W3C spec's job), and anything about how the resulting calls feel.
//
//   node tools/headless-gamepad-test.js      # from web/
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

let failures = 0;
function check(name, cond, detail) {
  if (cond) { console.log('  PASS ' + name); }
  else { failures++; console.log('  FAIL ' + name + (detail ? ' — ' + detail : '')); }
}

// --- sandbox --------------------------------------------------------------------------------
let padState = null;
let nowMs = 1000;
const listeners = {};
const sandbox = {
  console,
  navigator: { getGamepads: () => [padState] },
  performance: { now: () => nowMs },
  Math, isFinite,
};
sandbox.window = sandbox;
sandbox.window.addEventListener = (type, fn) => { (listeners[type] = listeners[type] || []).push(fn); };
vm.createContext(sandbox);
vm.runInContext(
  fs.readFileSync(path.join(__dirname, '..', 'public', 'eden-gamepad.js'), 'utf8'),
  sandbox, { filename: 'eden-gamepad.js' });

const G = sandbox.window.EdenGamepad;

// --- recording bridge -----------------------------------------------------------------------
let calls = [];
const settings = { gamepad: 1, gamepad_look_sensitivity: 1, gamepad_deadzone: 0.15 };
let blocked = false;
G.init({
  actionDown: (a) => calls.push(['down', a]),
  actionUp: (a) => calls.push(['up', a]),
  recomputeMove: () => calls.push(['recomputeMove']),
  applyLook: (dx, dy) => calls.push(['look', dx, dy]),
  holdActStart: (isBuild) => calls.push(['holdStart', isBuild]),
  holdActStop: () => calls.push(['holdStop']),
  hotbarScroll: (d) => calls.push(['scroll', d]),
  isBlocked: () => blocked,
  getSetting: (k) => settings[k],
  onConnect: (id, c) => calls.push(['connect', c]),
});

// --- helpers --------------------------------------------------------------------------------
function pad(opts) {
  opts = opts || {};
  const buttons = [];
  for (let i = 0; i < 16; i++) {
    const v = (opts.buttons && opts.buttons[i]) || 0;
    buttons.push({ pressed: v >= 1, value: v, touched: v > 0 });
  }
  return {
    // NOT `opts.mapping || 'standard'` — the non-standard case is exactly the falsy-string one a
    // real browser reports (`mapping: ''`), and that idiom would silently turn it back into
    // 'standard' and make the test assert nothing. It did, on the first run of this file.
    connected: true, index: 0, mapping: opts.mapping === undefined ? 'standard' : opts.mapping,
    id: 'Test Pad (STANDARD GAMEPAD)',
    axes: opts.axes || [0, 0, 0, 0],
    buttons,
  };
}
function tick(dtMs) { nowMs += (dtMs === undefined ? 16 : dtMs); calls = []; G.tick(); }
function has(kind, a) {
  return calls.some((c) => c[0] === kind && (a === undefined || c[1] === a));
}
function near(a, b, eps) { return Math.abs(a - b) <= (eps === undefined ? 1e-6 : eps); }

console.log('eden-gamepad translator');

// 1. No pad -> completely inert.
padState = null;
tick();
check('no pad: no calls at all', calls.length === 0, JSON.stringify(calls));

// 2. Deadzone: a stick inside the deadzone must produce nothing.
padState = pad({ axes: [0.1, 0.1, 0, 0] });
tick(); tick();   // two ticks: the first only seeds the look clock
check('inside deadzone: no movement', G.axes() === null);

// 3. Full tilt forward -> forward +1 (stick Y is +down, so -1 is forward).
padState = pad({ axes: [0, -1, 0, 0] });
tick();
check('full forward: forward = +1', G.axes() && near(G.axes().forward, 1));
check('full forward: strafe = 0', G.axes() && near(G.axes().strafe, 0));

// 4. Deadzone rescaling: half-tilt lands strictly between 0 and full, not clipped to either.
padState = pad({ axes: [0, -0.5, 0, 0] });
tick();
const halfF = G.axes().forward;
check('half tilt rescales into (0,1)', halfF > 0.3 && halfF < 0.95, 'got ' + halfF);

// 5. Releasing the stick reports one final zeroed frame, then goes quiet.
padState = pad({ axes: [0, 0, 0, 0] });
tick();
check('release: axes() null once settled', G.axes() === null);

// 6. Right stick look: sign and dt scaling.
padState = pad({ axes: [0, 0, 1, 0] });
tick(); // establishes the clock baseline for a known dt below
const before = calls.length;
tick(100);
const look = calls.find((c) => c[0] === 'look');
check('right stick right: positive dx', !!look && look[1] > 0, JSON.stringify(look));
check('right stick right: no dy', !!look && near(look[2], 0));
// 225 u/s * sens 1 * 0.1 s at full tilt (curve == 1 at magnitude 1) = 22.5
check('look magnitude matches LOOK_RATE*dt', !!look && near(look[1], 22.5, 0.01), look && look[1]);

// 7. Look step is clamped after a long stall (a hidden tab must not snap the camera).
tick(5000);
const bigLook = calls.find((c) => c[0] === 'look');
check('long stall clamped to MAX_LOOK_DT', !!bigLook && near(bigLook[1], 22.5, 0.01), bigLook && bigLook[1]);

// 8. Buttons are edge-triggered: down once on press, up once on release, nothing while held.
padState = pad({ buttons: { 0: 1 } });          // A -> jump
tick();
check('A press: jump down', has('down', 'jump'));
tick();
check('A held: no repeat', !has('down', 'jump') && !has('up', 'jump'));
padState = pad({});
tick();
check('A release: jump up', has('up', 'jump'));

// 9. Triggers route through hold-to-act, and only one owns the slot at a time.
padState = pad({ buttons: { 7: 1 } });          // RT -> mine
tick();
check('RT: holdActStart(mine)', calls.some((c) => c[0] === 'holdStart' && c[1] === false));
padState = pad({ buttons: { 7: 1, 6: 1 } });    // both triggers
tick();
check('LT while RT held: no second start', !has('holdStart'));
padState = pad({ buttons: { 6: 1 } });          // RT released, LT still down
tick();
check('RT release: holdActStop fires', has('holdStop'));
check('LT then takes the slot (build)', calls.some((c) => c[0] === 'holdStart' && c[1] === true));

// 10. Analog trigger below threshold must not fire.
padState = pad({ buttons: {} });
tick();
padState = pad({ buttons: { 7: 0.3 } });
tick();
check('trigger at 0.3: below threshold', !has('holdStart'));

// 11. Shoulders / d-pad scroll the hotbar, edge-triggered.
padState = pad({ buttons: { 5: 1 } });          // RB
tick();
check('RB: hotbar +1', calls.some((c) => c[0] === 'scroll' && c[1] === 1));
tick();
check('RB held: no repeat scroll', !has('scroll'));
padState = pad({ buttons: { 14: 1 } });         // D-pad left
tick();
check('D-pad left: hotbar -1', calls.some((c) => c[0] === 'scroll' && c[1] === -1));

// 12. A DOM panel blocks input and releases anything held.
padState = pad({ axes: [0, -1, 0, 0], buttons: { 0: 1 } });
tick();
blocked = true;
tick();
check('blocked: jump released', has('up', 'jump'));
check('blocked: movement zeroed', G.axes() === null);
tick();
check('blocked: stays inert', calls.length === 0, JSON.stringify(calls));
blocked = false;

// 13. The `gamepad` setting off makes it inert even with a pad present.
padState = pad({ axes: [0, -1, 0, 0] });
tick();
settings.gamepad = 0;
tick();
check('setting off: goes inert', G.axes() === null);
settings.gamepad = 1;

// 14. A non-standard mapping is ignored rather than guessed at.
padState = pad({ mapping: '', axes: [0, -1, 0, 0], buttons: { 0: 1 } });
tick(); tick();
check('non-standard mapping ignored', G.axes() === null && !has('down', 'jump'));

// 15. Disconnect releases everything.
padState = pad({ axes: [0, -1, 0, 0], buttons: { 0: 1 } });
tick();
calls = [];
padState = null;
(listeners.gamepaddisconnected || []).forEach((fn) => fn({ gamepad: { index: 0, id: 'Test Pad' } }));
check('disconnect: jump released', has('up', 'jump'));
check('disconnect: movement zeroed', G.axes() === null);

console.log(failures ? '\nFAILED (' + failures + ')' : '\nALL PASS');
process.exit(failures ? 1 : 0);
