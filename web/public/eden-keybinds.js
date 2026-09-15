// eden-keybinds.js — audit row 28/C5 split: the ACTION -> physical-code(s) map, its
// localStorage persistence, and the rebind-capture protocol eden-settings.js's Keys tab drives.
// Pure data + persistence; does NOT decide what an action DOES when it fires (that dispatch table
// is eden-input.js's actionDown/actionUp, which calls codeToActions/MOMENTARY_ACTIONS/
// CONTINUOUS_ACTIONS declared below).
//
// Self-contained: nothing here depends on any other split file, so its position in eden-st.html's
// load order relative to eden-viewport.js/eden-hotbar.js does not matter — only that it loads
// before eden-input.js, which reads codeToActions/MOMENTARY_ACTIONS/CONTINUOUS_ACTIONS.
//
// --- PC controls audit Phase 5: keybind remapping -------------------------------------------
// Replaces the old hard-coded `switch (e.code)` blocks with an ACTION -> physical-code(s) lookup,
// so eden-settings.js's Keys tab can rebind a key without this file knowing about it. Defaults are
// exactly the pre-Phase-5 bindings (nothing changes for existing players) plus arrow keys as a
// secondary movement binding. Only the PRIMARY (index 0) code per action is user-rebindable from
// the panel — the secondary arrow-key bindings are fixed.
//
// Deliberate exception to "settings live in C, never in the JS" (RESUME-HERE): the C settings model
// stores floats only and NSUserDefaults persists NSNumber only (Settings_web.mm), so a code->action
// map cannot live there. Keybinds are a JS-owned localStorage blob under the same `eden.prefs.`
// prefix eden-storage.js and the hotbar strip use. Don't "fix" this into the C model — see
// Settings_web.mm's header for why that would actually be a regression.
'use strict';

const KEYBIND_STORAGE_KEY = 'eden.prefs.keybinds';
const DEFAULT_KEYMAP = {
  moveForward: ['KeyW', 'ArrowUp'],
  moveBack:    ['KeyS', 'ArrowDown'],
  moveLeft:    ['KeyA', 'ArrowLeft'],
  moveRight:   ['KeyD', 'ArrowRight'],
  sprint:      ['ShiftLeft', 'ShiftRight'],
  walk:        ['AltLeft', 'AltRight'],
  // Space is jump on foot AND ascend while flying (eden_set_jump ignores it while flying, so the
  // two never fight over vertical velocity) — one action serves both, matching the engine's own
  // convention (Input_web.mm's eden_set_jump comment) rather than inventing a second one.
  jump:        ['Space'],
  flyDown:     ['ControlLeft', 'ControlRight'],
  // Same physical key as flyDown deliberately: on foot this crouches (Ctrl doing nothing else
  // there), while flying it also nudges FLY_DOWN, which is harmless (crouch shrinks the collision
  // box regardless of fly state, same as noclip+crouch in Source).
  crouch:      ['ControlLeft', 'ControlRight'],
  // F = fire/burn tool (rburn), V = fly toggle. Was F/F (fire had no binding at all, and F was
  // overloaded onto fly) until a live playtest reported no way to reach the fire tool and
  // accidentally toggling fly mode instead — swapped per that feedback.
  fireTool:    ['KeyF'],
  flyToggle:   ['KeyV'],
  blockPicker: ['KeyE'],
  colorPicker: ['KeyC'],
  menu:        ['Escape'],
  settings:    ['KeyO'],
  blockPreview:['KeyB'],
  fullscreen:  ['KeyL'],
  hotbar1: ['Digit1'], hotbar2: ['Digit2'], hotbar3: ['Digit3'],
  hotbar4: ['Digit4'], hotbar5: ['Digit5'], hotbar6: ['Digit6'],
  hotbar7: ['Digit7'], hotbar8: ['Digit8'], hotbar9: ['Digit9'],
};
const ACTION_LABELS = {
  moveForward: 'Move forward', moveBack: 'Move back',
  moveLeft: 'Strafe left', moveRight: 'Strafe right',
  sprint: 'Sprint', walk: 'Walk (slow)', jump: 'Jump / fly up', flyDown: 'Fly down',
  crouch: 'Crouch',
  fireTool: 'Fire tool', flyToggle: 'Toggle fly mode', blockPicker: 'Block picker',
  colorPicker: 'Colour picker',
  menu: 'Menu / pause', settings: 'Open settings', blockPreview: 'Toggle block preview',
  fullscreen: 'Fullscreen',
  hotbar1: 'Hotbar 1', hotbar2: 'Hotbar 2', hotbar3: 'Hotbar 3', hotbar4: 'Hotbar 4',
  hotbar5: 'Hotbar 5', hotbar6: 'Hotbar 6', hotbar7: 'Hotbar 7', hotbar8: 'Hotbar 8',
  hotbar9: 'Hotbar 9',
};
// Actions whose keydown must not repeat-fire under OS auto-repeat: one-shot toggles (mode toggles,
// panel opens) AND the begin/end tap actions (holding the key down past the OS's repeat-delay must
// not re-tap the button every ~30ms — a real regression caught in a live playtest: the original
// pre-Phase-5 code had one blanket `if(e.repeat) return` covering every action, and splitting
// dispatch by action here silently dropped that guard for anything not listed below). The
// continuous ones (movement/sprint/walk/fly) are Set membership (CONTINUOUS_ACTIONS, below), so a
// repeat keydown is a harmless no-op re-add and are correctly NOT in this set.
const MOMENTARY_ACTIONS = new Set([
  'flyToggle', 'fireTool', 'blockPicker', 'colorPicker', 'menu',
  'blockPreview', 'settings', 'fullscreen',
  'hotbar1','hotbar2','hotbar3','hotbar4','hotbar5','hotbar6','hotbar7','hotbar8','hotbar9',
]);
// Actions eden-input.js tracks as a held Set rather than dispatching once per keydown — declared
// here (beside MOMENTARY_ACTIONS, its natural counterpart) even though only eden-input.js reads it.
const CONTINUOUS_ACTIONS = new Set([
  'moveForward', 'moveBack', 'moveLeft', 'moveRight', 'sprint', 'walk', 'jump', 'flyDown', 'crouch',
]);

function loadKeymap() {
  let stored = null;
  try { stored = JSON.parse(localStorage.getItem(KEYBIND_STORAGE_KEY) || 'null'); } catch (e) {}
  const map = {};
  for (const action in DEFAULT_KEYMAP) {
    map[action] = (stored && Array.isArray(stored[action]) && stored[action].length)
      ? stored[action].slice() : DEFAULT_KEYMAP[action].slice();
  }
  return map;
}
let KEYMAP = loadKeymap();
function saveKeymap() {
  try { localStorage.setItem(KEYBIND_STORAGE_KEY, JSON.stringify(KEYMAP)); } catch (e) {}
}
function codeToActions(code) {
  const out = [];
  for (const action in KEYMAP) if (KEYMAP[action].indexOf(code) !== -1) out.push(action);
  return out;
}
// Used by eden-settings.js's Keys tab. Only slot 0 (the primary binding) is rebindable from the
// panel; secondary bindings (arrow keys) are not exposed there.
// Rebind UI support: the Keys tab calls startCapture(cb) after a "Press a key…" prompt; the NEXT
// keydown anywhere is consumed here (capture phase + stopPropagation, so it never reaches the real
// action-dispatch listener in eden-input.js or types into anything) and handed to `cb` instead of
// being interpreted as input.
let keyCaptureCallback = null;
window.addEventListener('keydown', (e) => {
  if (!keyCaptureCallback) return;
  e.preventDefault();
  e.stopPropagation();
  const cb = keyCaptureCallback;
  keyCaptureCallback = null;
  cb(e.code);
}, true);

window.EdenKeybinds = {
  actions: Object.keys(DEFAULT_KEYMAP),
  labelFor: (action) => ACTION_LABELS[action] || action,
  primaryCode: (action) => (KEYMAP[action] && KEYMAP[action][0]) || '',
  startCapture: (onCode) => { keyCaptureCallback = onCode; },
  cancelCapture: () => { keyCaptureCallback = null; },
  rebind: (action, code) => {
    if (!KEYMAP[action]) return;
    KEYMAP[action][0] = code;
    saveKeymap();
  },
  resetDefaults: () => {
    for (const action in DEFAULT_KEYMAP) KEYMAP[action] = DEFAULT_KEYMAP[action].slice();
    saveKeymap();
  },
};
